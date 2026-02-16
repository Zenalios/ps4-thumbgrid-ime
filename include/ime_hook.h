/**
 * @file ime_hook.h
 * @brief PS4 IME Dialog hooking definitions and structures
 */

#ifndef IME_HOOK_H
#define IME_HOOK_H

#include <stdint.h>
#include <stdbool.h>

/* ─── System Module IDs ───────────────────────────────────────────── */

#define SCE_SYSMODULE_IME_DIALOG               0x0096
#define SCE_SYSMODULE_INTERNAL_COMMON_DIALOG    0x80000018

/* ─── IME Dialog Enums ────────────────────────────────────────────── */

typedef enum OrbisImeDialogStatus {
    ORBIS_IME_DIALOG_STATUS_NONE     = 0,
    ORBIS_IME_DIALOG_STATUS_RUNNING  = 1,
    ORBIS_IME_DIALOG_STATUS_FINISHED = 2,
} OrbisImeDialogStatus;

typedef enum OrbisImeDialogEndStatus {
    ORBIS_IME_DIALOG_END_STATUS_OK             = 0,
    ORBIS_IME_DIALOG_END_STATUS_USER_CANCELED  = 1,
    ORBIS_IME_DIALOG_END_STATUS_ABORTED        = 2,
} OrbisImeDialogEndStatus;

typedef enum OrbisImePanelType {
    ORBIS_IME_PANEL_TYPE_DEFAULT     = 0,
    ORBIS_IME_PANEL_TYPE_BASIC_LATIN = 1,
    ORBIS_IME_PANEL_TYPE_URL         = 2,
    ORBIS_IME_PANEL_TYPE_MAIL        = 3,
    ORBIS_IME_PANEL_TYPE_NUMBER      = 4,
} OrbisImePanelType;

/* ─── IME Dialog Structures ───────────────────────────────────────── */

/*
 * Layout must match the real OrbisImeDialogSetting from the PS4 SDK.
 * Key differences from a naive guess:
 *   - supported_languages is uint64_t (8 bytes), not int32_t
 *   - filter is a function pointer (8 bytes), not int32_t
 *   - reserved is 16 bytes, not 32
 */
typedef struct OrbisImeDialogParam {
    int32_t  user_id;               /* offset  0 */
    int32_t  type;                  /* offset  4 */
    uint64_t supported_languages;   /* offset  8  (8 bytes!) */
    int32_t  enter_label;           /* offset 16 */
    int32_t  input_method;          /* offset 20 */
    void    *filter;                /* offset 24  (function ptr, 8 bytes!) */
    uint32_t option;                /* offset 32 */
    uint32_t max_text_length;       /* offset 36 */
    uint16_t *input_text_buffer;    /* offset 40 */
    float    posx;                  /* offset 48 */
    float    posy;                  /* offset 52 */
    int32_t  horizontal_alignment;  /* offset 56 */
    int32_t  vertical_alignment;    /* offset 60 */
    const uint16_t *placeholder;    /* offset 64 */
    const uint16_t *title;          /* offset 72 */
    int8_t   reserved[16];          /* offset 80 */
} OrbisImeDialogParam;              /* total: 96 bytes */

typedef struct OrbisImeDialogResult {
    int32_t end_status;
    int8_t  reserved[12];
} OrbisImeDialogResult;

/* ─── Function Pointer Types ──────────────────────────────────────── */

typedef int32_t (*sceImeDialogInit_t)(const OrbisImeDialogParam *param, void *param_extended);
typedef OrbisImeDialogStatus (*sceImeDialogGetStatus_t)(void);
typedef int32_t (*sceImeDialogGetResult_t)(OrbisImeDialogResult *result);
typedef int32_t (*sceImeDialogTerm_t)(void);

/* ─── Hook State ──────────────────────────────────────────────────── */

typedef struct ImeHookState {
    bool initialized;
    bool hooks_installed;
    sceImeDialogInit_t      original_init;
    sceImeDialogGetStatus_t original_get_status;
    sceImeDialogGetResult_t original_get_result;
    sceImeDialogTerm_t      original_term;
} ImeHookState;

/* ─── Public API ──────────────────────────────────────────────────── */

int32_t ime_hook_install(void);
int32_t ime_hook_remove(void);
const ImeHookState *ime_hook_get_state(void);

#endif /* IME_HOOK_H */
