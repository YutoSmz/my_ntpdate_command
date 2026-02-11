/*
    *  main.c
    *
    *  Created on: 2026/2/11
    *  Written by: Yuto Shimizu
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "ntp.h"

#ifndef NTP_TRY_COUNT
#define NTP_TRY_COUNT 8
#endif

// 時刻設定を試みる関数
static void maybe_set_time(ntp_offset_delay_t *offset_delay) {
#ifdef ENABLE_SETTIMEOFDAY
    struct timeval tv;
    struct timespec ts_now;
    clock_gettime(CLOCK_REALTIME, &ts_now);

    tv.tv_sec = ts_now.tv_sec + offset_delay->offset_sec;
    tv.tv_usec = (ts_now.tv_nsec + offset_delay->offset_nsec) / 1000;

    if (settimeofday(&tv, NULL) != 0) {
        perror("settimeofday failed");
    } else {
        fprintf(stderr, "Time updated by settimeofday\n");
    }
#else
    fprintf(stderr, "settimeofday is disabled in this build\n");
    fprintf(stderr, "If you want to enable it, define ENABLE_SETTIMEOFDAY=yes and recompile\n");
#endif
}

// メイン関数
int main(int argc, char *argv[]) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        fprintf(stderr, "Usage: %s <ntp_server>\n", argv[0]);
        fprintf(stderr, "<ntp_server>: NTP server address or hostname.\n");
        fprintf(stderr, "Example: %s ntp.nict.jp\n", argv[0]); // pool.ntp.org
        return EXIT_FAILURE;
    }
    LOGI("DEBUG LEVEL: %d", LOG_LEVEL);
    LOGI("NTP TRY COUNT: %d", NTP_TRY_COUNT);

    ntp_offset_delay_t results[NTP_TRY_COUNT];

    // NTPサーバに複数回時刻同期を要求.  FYI: ntpdateコマンドは8回試行する
    for (int i = 0; i < NTP_TRY_COUNT; i++) {
        LOGI("##### NTP request try %d/%d", i + 1, NTP_TRY_COUNT);
        // NTP要求
        if (ntp_request(argv[1], &results[i]) == 0) {
            LOGI("NTP request succeeded");
            LOGI("Offset: %ld sec, %ld nsec", results[i].offset_sec, results[i].offset_nsec);
            LOGI("Delay: %ld sec, %ld nsec", results[i].delay_sec, results[i].delay_nsec);
        } else {
            LOGW("NTP request failed, retrying...");
            results[i].offset_sec = 0;
            results[i].offset_nsec = 0;
            results[i].delay_sec = 0;
            results[i].delay_nsec = 0;
        }
    }

    int best_index = 0;
    // 最も遅延が小さい結果を選択
    for (int i = 1; i < NTP_TRY_COUNT; i++) {
        if (results[i].delay_sec == 0 && results[i].delay_nsec == 0) {
            continue; // 失敗した試行はスキップ
        } else {
            int64_t best_delay_total = results[best_index].delay_sec * 1000000000L + results[best_index].delay_nsec;
            int64_t current_delay_total = results[i].delay_sec * 1000000000L + results[i].delay_nsec;
            if (current_delay_total < best_delay_total) {
                best_index = i;
            }
        }
    }

    LOGI("Best result from try %d with delay %ld sec, %ld nsec", best_index + 1, results[best_index].delay_sec, results[best_index].delay_nsec);
    maybe_set_time(&results[best_index]);
    
    return EXIT_SUCCESS;
}
