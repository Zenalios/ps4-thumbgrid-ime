/**
 * @file ime_custom.h
 * @brief PSP-style cycling IME state machine and character sets
 */

#ifndef IME_CUSTOM_H
#define IME_CUSTOM_H

#include <stdint.h>
#include <stdbool.h>

#define IME_MAX_OUTPUT_LENGTH  256
#define IME_MAX_CHARSET_SIZE   96

static const char IME_DEFAULT_CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    " .,!?'-:;@#$%&*()";

static const char IME_NUMERIC_CHARSET[] = "0123456789.-+";

static const char IME_URL_CHARSET[] =
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    ".-_~:/?#[]@!$&'()*+,;=%";

typedef struct ImeCycleConfig {
    uint32_t initial_delay_ms;
    uint32_t repeat_interval_ms;
    uint32_t accel_threshold_ms;
    uint32_t accel_interval_ms;
} ImeCycleConfig;

typedef enum ImeCustomState {
    IME_STATE_INACTIVE = 0,
    IME_STATE_ACTIVE,
    IME_STATE_CONFIRMING,
    IME_STATE_CANCELLED,
} ImeCustomState;

typedef struct ImeSession {
    ImeCustomState state;
    const char    *charset;
    uint32_t       charset_length;
    uint32_t       cursor_index;
    uint16_t       output[IME_MAX_OUTPUT_LENGTH];
    uint32_t       output_length;
    uint32_t       max_output_length;
    uint32_t       text_cursor;    /* position within output buffer, 0 to output_length */
    bool           selected_all;   /* true = all text selected, next input replaces */
    uint32_t       sel_start;      /* selection range start (== sel_end means no partial selection) */
    uint32_t       sel_end;        /* selection range end */
    uint64_t       last_cycle_time_us;
    uint64_t       hold_start_time_us;
    bool           dpad_held;
    int8_t         hold_direction;
    ImeCycleConfig cycle_config;
    uint16_t      *caller_buffer;
    int32_t        panel_type;
    /* Internal clipboard for cut/copy/paste */
    uint16_t       clipboard[IME_MAX_OUTPUT_LENGTH];
    uint32_t       clipboard_length;
} ImeSession;

int32_t ime_session_init(ImeSession *session, int32_t panel_type,
    uint32_t max_length, uint16_t *caller_buffer, const uint16_t *prefill);
void    ime_session_cycle(ImeSession *session, int8_t delta);
bool    ime_session_confirm_char(ImeSession *session);
bool    ime_session_add_char(ImeSession *session, char c);
bool    ime_session_add_char16(ImeSession *session, uint16_t c);
bool    ime_session_backspace(ImeSession *session);
void    ime_session_select_all(ImeSession *session);
void    ime_session_cursor_left(ImeSession *session);
void    ime_session_cursor_right(ImeSession *session);
void    ime_session_cursor_home(ImeSession *session);
void    ime_session_cursor_end(ImeSession *session);
void    ime_session_delete_selection(ImeSession *session);
void    ime_session_set_selection(ImeSession *session, uint32_t start, uint32_t end);
void    ime_session_clear_selection(ImeSession *session);
void    ime_session_submit(ImeSession *session);
void    ime_session_cancel(ImeSession *session);
void    ime_session_copy(ImeSession *session);
void    ime_session_cut(ImeSession *session);
void    ime_session_paste(ImeSession *session);
char    ime_session_current_char(const ImeSession *session);
void    ime_session_get_neighbors(const ImeSession *session,
            char *prev_out, char *next_out);
void    ime_session_update_timing(ImeSession *session, uint64_t current_us);

#endif /* IME_CUSTOM_H */
