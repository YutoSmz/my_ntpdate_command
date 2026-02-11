#pragma once
#include <stdio.h>

#define LOG_ERROR 1
#define LOG_WARN  2
#define LOG_INFO  3
#define LOG_DEBUG 4

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_ERROR
#endif

#define LOG_PRINT(level, fmt, ...) \
    do { \
        if (level <= LOG_LEVEL) { \
            fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define LOGE(fmt, ...) LOG_PRINT(LOG_ERROR, "[ERROR] " fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_PRINT(LOG_WARN,  "[WARN ] " fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG_PRINT(LOG_INFO,  "[INFO ] " fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) LOG_PRINT(LOG_DEBUG, "[DEBUG] " fmt, ##__VA_ARGS__)
