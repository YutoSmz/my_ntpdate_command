/*
    *  ntp.c
    *
    *  Created on: 2026/2/11
    *  Written by: Yuto Shimizu
*/

#include "log.h"
#include "ntp.h"

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#define NTP_PORT_STR "123"
#define NTP_PACKET_SIZE 48
#define RECEIVE_TIMEOUT_SEC 2 // 受信タイムアウト時間[秒]
#define NTP_UNIX_EPOCH_DIFF 2208988800ULL  // 1900->1970 offset [seconds]
// ローカル時刻はUNIX時刻は1970年1月1日 00:00:00 UTCが基準だが、NTP時刻は1900年1月1日 00:00:00 UTCが基準。
// NTP epoch(1900-01-01) と UNIX epoch(1970-01-01) の差は 2,208,988,800 秒

// NTPヘッダのビットフィールド定義
#define NTP_LI_SHIFT   6
#define NTP_VN_SHIFT   3
#define NTP_MODE_SHIFT 0

#define NTP_LI_MASK    0xC0  // 1100_0000
#define NTP_VN_MASK    0x38  // 0011_1000
#define NTP_MODE_MASK  0x07  // 0000_0111

#define NTP_SET_LI(h, li)    ((h)->li_vn_mode = (uint8_t)(((h)->li_vn_mode & ~NTP_LI_MASK)   | (((li) & 0x3) << NTP_LI_SHIFT)))
#define NTP_SET_VN(h, vn)    ((h)->li_vn_mode = (uint8_t)(((h)->li_vn_mode & ~NTP_VN_MASK)   | (((vn) & 0x7) << NTP_VN_SHIFT)))
#define NTP_SET_MODE(h, md)  ((h)->li_vn_mode = (uint8_t)(((h)->li_vn_mode & ~NTP_MODE_MASK) | (((md) & 0x7) << NTP_MODE_SHIFT)))

#define NTP_GET_LI(h)   (((h)->li_vn_mode & NTP_LI_MASK)   >> NTP_LI_SHIFT)
#define NTP_GET_VN(h)   (((h)->li_vn_mode & NTP_VN_MASK)   >> NTP_VN_SHIFT)
#define NTP_GET_MODE(h) (((h)->li_vn_mode & NTP_MODE_MASK) >> NTP_MODE_SHIFT)

/* RFC 5905 NTP packet format
      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |LI | VN  |Mode |    Stratum     |     Poll      |  Precision   |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                         Root Delay                            |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                         Root Dispersion                       |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                          Reference ID                         |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      +                     Reference Timestamp (64)                  +
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      +                      Origin Timestamp (64)                    +
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      +                      Receive Timestamp (64)                   +
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      +                      Transmit Timestamp (64)                  +
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      .                    Extension Field 1 (variable)               .
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      .                    Extension Field 2 (variable)               .
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                          Key Identifier                       |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                            dgst (128)                         |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

#pragma pack(push, 1)  // 構造体のパディング禁止
typedef struct ntp_packet {
    // 1バイト目
    uint8_t li_vn_mode; // li: 閏秒指示子、vn: バージョン、mode: 動作モード（クライアントモードは0b011、サーバモードは0b100）
    // 
    uint8_t stratum; // ストラタム（00000010 ~ 00001111、2次〜15次参照まで。それ以外は予約領域）
    // stratum1をPrimaryサーバ、stratum2-15を、Secondaryサーバという
    int8_t poll; // ポーリング間隔 （2^poll秒）、連続するメッセージの最大間隔
    int8_t precision; // ローカル時計の精度 (2^precision 秒)

    int32_t root_delay; // 固定小数16.16形式。1次参照源までの往復時間の合計
    uint32_t root_dispersion; // 1次参照源までの相対的な誤差
    uint32_t ref_id; // 時刻源のIPv4のIPアドレス
    // Reference Timestamp: Time when the system clock was last set or corrected
    // ローカル時計が最後に設定・修正された時刻
    uint32_t ref_ts_sec; 
    uint32_t ref_ts_frac; 
    // Origin Timestamp: Time at the client when the request departed for the server
    // クライアントからサーバへリクエストを発信した時刻（T1）
    uint32_t org_ts_sec;
    uint32_t org_ts_frac;
    // Receive Timestamp: Time at the server when the request arrived from the client
    // サーバへリクエストが到達した時刻（T2）
    uint32_t rx_ts_sec;
    uint32_t rx_ts_frac;
    // Transmit Timestamp: Time at the server when the response left for the client
    // サーバからクライアントへ応答が発信された時刻（T3）
    uint32_t tx_ts_sec;
    uint32_t tx_ts_frac;
} ntp_packet_t;
#pragma pack(pop)


// prototype declarations
static inline uint64_t ntp_from_timespec(struct timespec ts);
static inline struct timespec timespec_from_ntp(uint32_t sec, uint32_t frac);
int get_sockaddr_info(const char *hostnm, const char *portnm, struct sockaddr_storage *saddr, socklen_t *saddr_len);
int udp_send_recv(char *addr, char *port, void *send_buf, size_t send_buf_size, void *recv_buf, size_t recv_buf_size);
int ntp_print_packet(ntp_packet_t *packet);
int ntp_print_raw_packet(char *buf, size_t buf_size);


static inline uint64_t ntp_from_timespec(struct timespec ts) {
    uint64_t sec = (uint64_t)(ts.tv_sec + (uint64_t)NTP_UNIX_EPOCH_DIFF);
    uint64_t frac = (uint64_t)((((uint64_t)ts.tv_nsec) << 32) / 1000000000ULL);
    return (sec << 32) | frac; // 上位32ビットが秒、下位32ビットが小数部
}

static inline struct timespec timespec_from_ntp(uint32_t sec, uint32_t frac) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ntohl(sec) - (uint64_t)NTP_UNIX_EPOCH_DIFF);
    ts.tv_nsec = (long)((((uint64_t)ntohl(frac)) * 1000000000ULL) >> 32);
    return ts;
}

static inline int64_t ts_to_ns(struct timespec t) {
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

static inline void ns_to_ts(int64_t ns, int64_t *sec, int64_t *nsec) {
    *sec = ns / 1000000000LL;
    *nsec = ns % 1000000000LL;
    if (*nsec < 0) {
        *nsec += 1000000000LL;
        *sec -= 1;
    }
}


int get_sockaddr_info(const char *hostnm, const char *portnm, struct sockaddr_storage *saddr, socklen_t *saddr_len) {
    struct addrinfo hints, *res0;
    int errcode;

    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if ((errcode = getaddrinfo(hostnm, portnm, &hints, &res0)) != 0){
        LOGE("getaddrinfo():%s", gai_strerror(errcode));
        freeaddrinfo(res0);
        return(-1);
    }
    LOGD("addr=%s", hostnm);
    LOGD("port=%s", portnm);
    (void) memcpy(saddr, res0->ai_addr, res0->ai_addrlen);
    *saddr_len = res0->ai_addrlen;
    return (0);
}

int udp_send_recv(char *addr, char *port, void *send_buf, size_t send_buf_size, void *recv_buf, size_t recv_buf_size) {
    int sockfd;
    struct sockaddr_storage from, to;
    struct timeval timeout;
    size_t len;
    socklen_t fromlen, tolen;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        LOGE("NTP socket error (%d:%s)", errno, strerror(errno));
        return -1;
    }

    // タイムアウトを設定
    timeout.tv_sec = RECEIVE_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    // 送信先アドレス情報の取得
    if (get_sockaddr_info(addr, port, &to, &tolen) == -1) {
        LOGE("get_sockaddr_info():error");
        close(sockfd);
        return -1;
    }

    // パケットをUDPで送信
    if (sendto(sockfd, send_buf, send_buf_size, 0, (struct sockaddr *)&to, tolen) == -1) {
        LOGE("NTP sendto error (%d:%s)", errno, strerror(errno));
        close(sockfd);
        return -1;
    }

    // 応答の受信
    fromlen = sizeof(from);
    if ((len = recvfrom(sockfd, recv_buf, recv_buf_size, 0, (struct sockaddr *)&from, &fromlen)) == -1) {
        LOGE("NTP recvfrom error (%d:%s)", errno, strerror(errno));
        close(sockfd);
        return -1;
    }

    #if LOG_LEVEL >= LOG_INFO
    char nbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    int errcode;
    if ((errcode = getnameinfo((struct sockaddr *) &from, fromlen, nbuf, sizeof(nbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        LOGE("getnameinfo():%s", gai_strerror(errcode));
    } else {
        LOGI("Received NTP response from %s:%s, size=%zu bytes", nbuf, sbuf, len);
    }
    #endif

    close(sockfd);
    return (int)len;
}

// NTP要求関数
int ntp_request(char *ntp_server, ntp_offset_delay_t *result) {
    char send_buf[NTP_PACKET_SIZE];
    char recv_buf[NTP_PACKET_SIZE];

    memset(send_buf, 0, sizeof(send_buf));
    memset(recv_buf, 0, sizeof(recv_buf));
    ntp_packet_t *send_packet = (ntp_packet_t *)send_buf;
    ntp_packet_t *recv_packet = (ntp_packet_t *)recv_buf;
    // NTPリクエストパケットの作成
    NTP_SET_LI(send_packet, 0);   // LI: 0
    NTP_SET_VN(send_packet, 4);   // VN: 4
    NTP_SET_MODE(send_packet, 3); // Mode: 3 (Client)

    // 送信前の時刻を取得し、Transmit Timestampに設定
    struct timespec ts_now;
    clock_gettime(CLOCK_REALTIME, &ts_now);
    uint64_t tx_ts = ntp_from_timespec(ts_now);
    send_packet->tx_ts_sec = htonl((uint32_t)(tx_ts >> 32));
    send_packet->tx_ts_frac = htonl((uint32_t)(tx_ts & 0xFFFFFFFF));

    // NTPサーバへUDPで送信し、応答を受信
    int recv_size = udp_send_recv(ntp_server, NTP_PORT_STR, send_buf, sizeof(send_buf), recv_buf, sizeof(recv_buf));
    if (recv_size == -1) {
        LOGE("NTP send/receive error");
        return -1;
    }
    if (recv_size < NTP_PACKET_SIZE) {
        LOGE("NTP response size too small: %d bytes", recv_size);
        return -1;
    }

    
    // 受信したNTPパケットの解析
    // 受信時の時刻を取得
    struct timespec t4;
    clock_gettime(CLOCK_REALTIME, &t4); // T4
    struct timespec t1 = timespec_from_ntp(recv_packet->org_ts_sec, recv_packet->org_ts_frac); // T1
    struct timespec t2 = timespec_from_ntp(recv_packet->rx_ts_sec, recv_packet->rx_ts_frac);   // T2
    struct timespec t3 = timespec_from_ntp(recv_packet->tx_ts_sec, recv_packet->tx_ts_frac);   // T3

    // オフセットと遅延の計算
    // offset = ((T2 - T1) + (T3 - T4)) / 2
    // delay  = (T4 - T1) - (T3 - T2)
    int64_t t1_ns = ts_to_ns(t1);
    int64_t t2_ns = ts_to_ns(t2);
    int64_t t3_ns = ts_to_ns(t3);
    int64_t t4_ns = ts_to_ns(t4);

    int64_t offset_ns = ((t2_ns - t1_ns) + (t3_ns - t4_ns)) / 2;
    int64_t delay_ns  = (t4_ns - t1_ns) - (t3_ns - t2_ns);

    int64_t offset_sec, offset_nsec;
    int64_t delay_sec, delay_nsec;
    ns_to_ts(offset_ns, &offset_sec, &offset_nsec);
    ns_to_ts(delay_ns,  &delay_sec,  &delay_nsec);

    #if LOG_LEVEL >= LOG_INFO
    (void) ntp_print_packet(send_packet);
    #endif
    #if LOG_LEVEL >= LOG_DEBUG
    (void) ntp_print_raw_packet(send_buf, sizeof(send_buf));
    #endif

    #if LOG_LEVEL >= LOG_INFO
    (void) ntp_print_packet(recv_packet);
    #endif
    #if LOG_LEVEL >= LOG_DEBUG
    (void) ntp_print_raw_packet(recv_buf, recv_size);
    #endif

    result->offset_sec = offset_sec;
    result->offset_nsec = offset_nsec;
    result->delay_sec = delay_sec;
    result->delay_nsec = delay_nsec;
    return 0;
}


// for dumping NTP packet
int ntp_print_packet(struct ntp_packet *packet) {
    if (packet == NULL) {
        LOGE("ntp_print_packet: packet is NULL");
        return -1;
    }

    fprintf(stderr, "NTP Packet:\n");
    fprintf(stderr, " LI: %u\n", NTP_GET_LI(packet));
    fprintf(stderr, " VN: %u\n", NTP_GET_VN(packet));
    fprintf(stderr, " Mode: %u\n", NTP_GET_MODE(packet));
    fprintf(stderr, " Stratum: %u\n", packet->stratum);
    fprintf(stderr, " Poll: %d\n", packet->poll);
    fprintf(stderr, " Precision: %d\n", packet->precision);
    fprintf(stderr, " Root Delay: %d\n", ntohl(packet->root_delay));
    fprintf(stderr, " Root Dispersion: %u\n", ntohl(packet->root_dispersion));
    fprintf(stderr, " Reference ID: %u\n", ntohl(packet->ref_id));
    fprintf(stderr, " Reference Timestamp: %u.%09u\n", ntohl(packet->ref_ts_sec), ntohl(packet->ref_ts_frac));
    fprintf(stderr, " Origin Timestamp: %u.%09u\n", ntohl(packet->org_ts_sec), ntohl(packet->org_ts_frac));
    fprintf(stderr, " Receive Timestamp: %u.%09u\n", ntohl(packet->rx_ts_sec), ntohl(packet->rx_ts_frac));
    fprintf(stderr, " Transmit Timestamp: %u.%09u\n", ntohl(packet->tx_ts_sec), ntohl(packet->tx_ts_frac));

    return 0;
}

// for dumping raw NTP packet
int ntp_print_raw_packet(char *buf, size_t buf_size) {
    if (buf == NULL) {
        LOGE("ntp_print_raw_packet: buf is NULL");
        return -1;
    }
    fprintf(stderr, "NTP Raw Packet:\n");
    for (size_t i = 0; i < buf_size; i++) {
        fprintf(stderr, "%02x ", (uint8_t)buf[i]);
        if ((i + 1) % 16 == 0) {
            fprintf(stderr, "\n");
        }
    }
    if (buf_size % 16 != 0) {
        fprintf(stderr, "\n");
    }
    return 0;
}