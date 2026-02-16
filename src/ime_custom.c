/**
 * @file ime_custom.c
 * @brief PSP-style cycling IME session implementation
 */

#include <string.h>

#include "plugin_common.h"
#include "ime_custom.h"
#include "ime_hook.h" /* for OrbisImePanelType */

/* ─── Helpers ─────────────────────────────────────────────────────── */

static const char *charset_for_panel(int32_t panel_type, uint32_t *out_length) {
    const char *cs;

    switch (panel_type) {
    case ORBIS_IME_PANEL_TYPE_NUMBER:
        cs = IME_NUMERIC_CHARSET;
        break;
    case ORBIS_IME_PANEL_TYPE_URL:
    case ORBIS_IME_PANEL_TYPE_MAIL:
        cs = IME_URL_CHARSET;
        break;
    default:
        cs = IME_DEFAULT_CHARSET;
        break;
    }

    uint32_t len = 0;
    while (cs[len] != '\0' && len < IME_MAX_CHARSET_SIZE) {
        len++;
    }
    *out_length = len;
    return cs;
}

static uint32_t wrap_index(int64_t index, uint32_t length) {
    if (length == 0) {
        return 0;
    }
    int64_t wrapped = index % (int64_t)length;
    if (wrapped < 0) {
        wrapped += (int64_t)length;
    }
    return (uint32_t)wrapped;
}

/* ─── Session Lifecycle ───────────────────────────────────────────── */

int32_t ime_session_init(
    ImeSession *session,
    int32_t panel_type,
    uint32_t max_length,
    uint16_t *caller_buffer,
    const uint16_t *prefill
) {
    if (!session) {
        return IME_ERROR_INVALID_PARAM;
    }
    if (max_length == 0 || max_length > IME_MAX_OUTPUT_LENGTH) {
        max_length = CLAMP(max_length, 1, IME_MAX_OUTPUT_LENGTH);
    }

    memset(session, 0, sizeof(ImeSession));
    session->state = IME_STATE_ACTIVE;
    session->panel_type = panel_type;
    session->caller_buffer = caller_buffer;
    session->max_output_length = max_length;
    session->charset = charset_for_panel(panel_type, &session->charset_length);
    session->cursor_index = 0;

    session->cycle_config = (ImeCycleConfig){
        .initial_delay_ms   = 400,
        .repeat_interval_ms = 150,
        .accel_threshold_ms = 1500,
        .accel_interval_ms  = 50,
    };

    if (prefill) {
        uint32_t prefill_len = safe_u16_strlen(prefill, max_length);
        safe_u16_copy(session->output, prefill, max_length);
        session->output_length = prefill_len;
        session->text_cursor   = prefill_len;
    }

    LOG_INFO("IME session: charset=%u chars, max=%u, prefill=%u",
        session->charset_length, max_length, session->output_length);
    return IME_OK;
}

/* ─── Cycling ─────────────────────────────────────────────────────── */

void ime_session_cycle(ImeSession *session, int8_t delta) {
    if (!session || session->state != IME_STATE_ACTIVE) {
        return;
    }
    if (session->charset_length == 0) {
        return;
    }
    int64_t new_idx = (int64_t)session->cursor_index + delta;
    session->cursor_index = wrap_index(new_idx, session->charset_length);
}

bool ime_session_confirm_char(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) {
        return false;
    }
    if (session->output_length >= session->max_output_length) {
        return false;
    }
    if (session->cursor_index >= session->charset_length) {
        return false;
    }

    char c = session->charset[session->cursor_index];
    session->output[session->output_length] = (uint16_t)c;
    session->output_length++;
    session->output[session->output_length] = 0;
    session->cursor_index = 0;

    LOG_DEBUG("Confirmed '%c', len=%u", c, session->output_length);
    return true;
}

/* Helper: delete any active selection (select-all or partial) before input */
static void clear_if_selected(ImeSession *session) {
    if (session->selected_all) {
        session->output_length = 0;
        session->text_cursor = 0;
        session->output[0] = 0;
        session->selected_all = false;
        session->sel_start = session->sel_end = 0;
        return;
    }
    /* Delete partial selection if active */
    if (session->sel_start != session->sel_end) {
        ime_session_delete_selection(session);
    }
}

bool ime_session_add_char(ImeSession *session, char c) {
    return ime_session_add_char16(session, (uint16_t)c);
}

bool ime_session_add_char16(ImeSession *session, uint16_t c) {
    if (!session || session->state != IME_STATE_ACTIVE) {
        return false;
    }

    /* If any text is selected, delete it first */
    clear_if_selected(session);

    if (session->output_length >= session->max_output_length) {
        return false;
    }

    /* Insert at text_cursor, shift chars right */
    uint32_t pos = session->text_cursor;
    if (pos > session->output_length) pos = session->output_length;

    for (uint32_t i = session->output_length; i > pos; i--) {
        session->output[i] = session->output[i - 1];
    }
    session->output[pos] = c;
    session->output_length++;
    session->text_cursor = pos + 1;
    session->output[session->output_length] = 0;

    LOG_DEBUG("Added char at %u, len=%u", pos, session->output_length);
    return true;
}

bool ime_session_backspace(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) {
        return false;
    }

    /* If all text is selected, clear everything */
    if (session->selected_all) {
        session->output_length = 0;
        session->text_cursor = 0;
        session->output[0] = 0;
        session->selected_all = false;
        session->sel_start = session->sel_end = 0;
        return true;
    }

    /* If partial selection, delete it */
    if (session->sel_start != session->sel_end) {
        ime_session_delete_selection(session);
        return true;
    }

    if (session->text_cursor == 0 || session->output_length == 0) {
        return false;
    }

    /* Delete char at text_cursor - 1, shift chars left */
    uint32_t pos = session->text_cursor - 1;
    for (uint32_t i = pos; i < session->output_length - 1; i++) {
        session->output[i] = session->output[i + 1];
    }
    session->output_length--;
    session->text_cursor = pos;
    session->output[session->output_length] = 0;
    return true;
}

/* ─── Cursor Movement ─────────────────────────────────────────────── */

void ime_session_cursor_left(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    session->selected_all = false;
    if (session->text_cursor > 0) {
        session->text_cursor--;
    }
}

void ime_session_cursor_right(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    session->selected_all = false;
    if (session->text_cursor < session->output_length) {
        session->text_cursor++;
    }
}

void ime_session_cursor_home(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    session->selected_all = false;
    session->text_cursor = 0;
}

void ime_session_cursor_end(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    session->selected_all = false;
    session->text_cursor = session->output_length;
}

/* ─── Selection ───────────────────────────────────────────────────── */

void ime_session_set_selection(ImeSession *session, uint32_t start, uint32_t end) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    if (start > session->output_length) start = session->output_length;
    if (end > session->output_length) end = session->output_length;
    if (start > end) { uint32_t t = start; start = end; end = t; }
    session->sel_start = start;
    session->sel_end = end;
    session->selected_all = false;
}

void ime_session_clear_selection(ImeSession *session) {
    if (!session) return;
    session->sel_start = 0;
    session->sel_end = 0;
    session->selected_all = false;
}

void ime_session_delete_selection(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    uint32_t s = session->sel_start;
    uint32_t e = session->sel_end;
    if (s == e) return;
    if (s > e) { uint32_t t = s; s = e; e = t; }
    if (e > session->output_length) e = session->output_length;
    if (s > session->output_length) return;
    uint32_t del_len = e - s;
    for (uint32_t i = s; i + del_len < session->output_length; i++) {
        session->output[i] = session->output[i + del_len];
    }
    session->output_length -= del_len;
    session->output[session->output_length] = 0;
    session->text_cursor = s;
    session->sel_start = 0;
    session->sel_end = 0;
    session->selected_all = false;
}

void ime_session_select_all(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    if (session->output_length == 0) return;
    session->selected_all = true;
    session->sel_start = 0;
    session->sel_end = session->output_length;
    session->text_cursor = session->output_length;
}

/* ─── Submit / Cancel ─────────────────────────────────────────────── */

void ime_session_submit(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) {
        return;
    }
    if (session->caller_buffer) {
        safe_u16_copy(session->caller_buffer, session->output,
            session->max_output_length);
    }
    session->state = IME_STATE_CONFIRMING;
    LOG_INFO("IME submitted: %u chars", session->output_length);
}

void ime_session_cancel(ImeSession *session) {
    if (!session) {
        return;
    }
    session->state = IME_STATE_CANCELLED;
    LOG_INFO("IME cancelled");
}

/* ─── Clipboard Operations ───────────────────────────────────────── */

void ime_session_copy(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;

    uint32_t s, e;
    if (session->selected_all) {
        s = 0;
        e = session->output_length;
    } else if (session->sel_start != session->sel_end) {
        s = session->sel_start;
        e = session->sel_end;
        if (s > e) { uint32_t t = s; s = e; e = t; }
    } else {
        return;  /* nothing selected */
    }

    uint32_t len = e - s;
    if (len > IME_MAX_OUTPUT_LENGTH) len = IME_MAX_OUTPUT_LENGTH;
    memcpy(session->clipboard, &session->output[s], len * sizeof(uint16_t));
    session->clipboard_length = len;
    LOG_DEBUG("Clipboard copy: %u chars", len);
}

void ime_session_cut(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;

    ime_session_copy(session);
    if (session->clipboard_length > 0) {
        if (session->selected_all) {
            session->output_length = 0;
            session->text_cursor = 0;
            session->output[0] = 0;
            session->selected_all = false;
            session->sel_start = session->sel_end = 0;
        } else {
            ime_session_delete_selection(session);
        }
    }
    LOG_DEBUG("Clipboard cut: %u chars in clipboard", session->clipboard_length);
}

void ime_session_paste(ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) return;
    if (session->clipboard_length == 0) return;

    /* Delete any selection first */
    if (session->selected_all) {
        session->output_length = 0;
        session->text_cursor = 0;
        session->output[0] = 0;
        session->selected_all = false;
        session->sel_start = session->sel_end = 0;
    } else if (session->sel_start != session->sel_end) {
        ime_session_delete_selection(session);
    }

    uint32_t avail = session->max_output_length - session->output_length;
    uint32_t paste_len = session->clipboard_length;
    if (paste_len > avail) paste_len = avail;
    if (paste_len == 0) return;

    uint32_t pos = session->text_cursor;
    if (pos > session->output_length) pos = session->output_length;

    /* Shift existing chars right */
    for (uint32_t i = session->output_length; i > pos; i--) {
        session->output[i + paste_len - 1] = session->output[i - 1];
    }
    /* Insert clipboard */
    memcpy(&session->output[pos], session->clipboard, paste_len * sizeof(uint16_t));
    session->output_length += paste_len;
    session->text_cursor = pos + paste_len;
    session->output[session->output_length] = 0;

    LOG_DEBUG("Clipboard paste: %u chars at pos %u", paste_len, pos);
}

/* ─── Display Helpers ─────────────────────────────────────────────── */

char ime_session_current_char(const ImeSession *session) {
    if (!session || session->state != IME_STATE_ACTIVE) {
        return 0;
    }
    if (session->cursor_index >= session->charset_length) {
        return 0;
    }
    return session->charset[session->cursor_index];
}

void ime_session_get_neighbors(
    const ImeSession *session, char *prev_out, char *next_out
) {
    if (!session || session->state != IME_STATE_ACTIVE ||
        session->charset_length == 0) {
        if (prev_out) *prev_out = 0;
        if (next_out) *next_out = 0;
        return;
    }
    uint32_t prev = wrap_index(
        (int64_t)session->cursor_index - 1, session->charset_length);
    uint32_t next = wrap_index(
        (int64_t)session->cursor_index + 1, session->charset_length);
    if (prev_out) *prev_out = session->charset[prev];
    if (next_out) *next_out = session->charset[next];
}

/* ─── Timing ──────────────────────────────────────────────────────── */

void ime_session_update_timing(ImeSession *session, uint64_t current_us) {
    if (!session || session->state != IME_STATE_ACTIVE) {
        return;
    }
    if (!session->dpad_held || session->hold_direction == 0) {
        return;
    }

    uint64_t held_ms  = (current_us - session->hold_start_time_us) / 1000;
    uint64_t since_ms = (current_us - session->last_cycle_time_us) / 1000;

    uint32_t interval = session->cycle_config.repeat_interval_ms;
    if (held_ms > session->cycle_config.accel_threshold_ms) {
        interval = session->cycle_config.accel_interval_ms;
    }

    if (held_ms < session->cycle_config.initial_delay_ms) {
        return;
    }
    if (since_ms < interval) {
        return;
    }

    ime_session_cycle(session, session->hold_direction);
    session->last_cycle_time_us = current_us;
}
