/**
 * @file plugin_common.h
 * @brief Shared definitions for the ThumbGrid IME GoldHEN plugin
 */

#ifndef PLUGIN_COMMON_H
#define PLUGIN_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ─── Plugin Identity ─────────────────────────────────────────────── */

#define PLUGIN_NAME       "ThumbGrid IME"
#define PLUGIN_DESC       "PSP-style cycling text input for PS4"
#define PLUGIN_AUTH       "ealcon"
#define PLUGIN_VER        0x00010000  /* 1.0.0 */

/* ─── System Module IDs (non-internal) ────────────────────────────── */

#define SCE_SYSMODULE_PAD            0x0021
#define SCE_SYSMODULE_USER_SERVICE   0x0012
#define SCE_SYSMODULE_VIDEO_OUT      0x0014

/* ─── Error Codes ─────────────────────────────────────────────────── */

#define IME_OK                     0
#define IME_ERROR_GENERIC         -1
#define IME_ERROR_NOT_INITIALIZED -2
#define IME_ERROR_HOOK_FAILED     -3
#define IME_ERROR_MODULE_LOAD     -4
#define IME_ERROR_NID_RESOLVE     -5
#define IME_ERROR_INVALID_PARAM   -6
#define IME_ERROR_BUFFER_FULL     -7

/* ─── Logging ─────────────────────────────────────────────────────── */

#include <stdio.h>

#define LOG_PREFIX "[TGIME] "

#define LOG_INFO(fmt, ...)  \
    printf(LOG_PREFIX "INFO: " fmt "\n", ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)  \
    printf(LOG_PREFIX "WARN: " fmt "\n", ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    printf(LOG_PREFIX "ERROR: " fmt "\n", ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    printf(LOG_PREFIX "DBG: " fmt "\n", ##__VA_ARGS__)

/* ─── On-Screen Notification (implemented in main.c) ─────────────── */

void notify(const char *fmt, ...);

/* ─── Utility Macros ──────────────────────────────────────────────── */

#define ARRAY_SIZE(arr)   (sizeof(arr) / sizeof((arr)[0]))

#define CLAMP(val, lo, hi) \
    ((val) < (lo) ? (lo) : ((val) > (hi) ? (hi) : (val)))

static inline uint32_t safe_u16_strlen(const uint16_t *str, uint32_t max_len) {
    if (!str) {
        return 0;
    }
    uint32_t len = 0;
    while (len < max_len && str[len] != 0) {
        len++;
    }
    return len;
}

static inline void safe_u16_copy(
    uint16_t *dst,
    const uint16_t *src,
    uint32_t max_dst_chars
) {
    if (!dst || max_dst_chars == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    uint32_t i = 0;
    uint32_t limit = max_dst_chars - 1;
    while (i < limit && src[i] != 0) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

#endif /* PLUGIN_COMMON_H */
