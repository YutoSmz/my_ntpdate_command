/*
    *  ntp.h
    *
    *  Created on: 2026/2/11
    *  Written by: Yuto Shimizu
*/

#ifndef __NTP_H__
#define __NTP_H__

#include <stdint.h>

typedef struct {
    int64_t offset_sec;
    int64_t offset_nsec;
    int64_t delay_sec;
    int64_t delay_nsec;
} ntp_offset_delay_t;

/**
 * @brief NTPサーバに時刻同期を要求する
 *
 * @param ntp_server NTPサーバのIPアドレスまたはホスト名
 * @param result NTPサーバからの応答をもとに算出したoffset、delay
 * @return int 失敗時は-1
 */
int ntp_request(char *ntp_server, ntp_offset_delay_t *result);

#endif // __NTP_H__
