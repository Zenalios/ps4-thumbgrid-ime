/**
 * @file ime_hook.c
 * @brief IME Dialog function hooking implementation
 *
 * Phase 3: ThumbGrid grid input with framebuffer overlay.
 * Analog stick selects cell, face buttons input characters.
 * Overlay renders via sceVideoOutSubmitFlip hook.
 * Falls back to notification display if overlay framebuffers unavailable.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "plugin_common.h"
#include "ime_hook.h"
#include "ime_custom.h"
#include "input.h"
#include "thumbgrid.h"
#include "overlay.h"
#include "thumbgrid_ipc.h"

#include <Detour.h>
#include <GoldHEN.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

/* ─── Constants ──────────────────────────────────────────────────── */

#define IME_DEFAULT_MAX_LENGTH  256

/* Startup grace period — ignore input for first 300ms after init */
#define IME_GRACE_PERIOD_US  300000

/* Notification fallback throttle */
#define IME_NOTIFY_INTERVAL_US  200000  /* 200ms between updates */
#define IME_NOTIFY_REQ_ID       0x4349  /* "CI" - fixed reqId for replacement */

/* ─── Static State ────────────────────────────────────────────────── */

static ImeHookState g_hook_state;
static ImeSession   g_session;
static ThumbGridState     g_tgrid;
static bool         g_custom_active = false;

/* Pad management */
static int32_t    g_pad_handle  = -1;
static bool       g_owns_pad    = false;
static int32_t    g_user_id     = -1;
static InputState g_input_state;

/* Startup grace period */
static uint64_t g_session_start_us = 0;

/* Notification fallback state */
static uint64_t g_last_notify_time_us = 0;
static uint32_t g_last_display_hash   = 0;

/* Cached screen dimensions (set by draw callback) */
static uint32_t g_overlay_screen_w = 1920;
static uint32_t g_overlay_screen_h = 1080;

/* Backspace hold-to-repeat state */
static bool     g_bs_held          = false;
static uint64_t g_bs_start_us      = 0;
static uint64_t g_bs_last_repeat_us = 0;

/* X (cross) hold state for text selection */
static bool     g_x_held           = false;
static bool     g_x_dpad_used      = false;  /* D-pad was pressed during X hold */
static uint32_t g_x_anchor         = 0;      /* cursor pos when X was pressed */

/* L2 analog trigger state for shift hold */
static uint8_t  g_l2_prev_analog   = 0;
static bool     g_l2_shift_active  = false;  /* temporary shift currently applied */
static int32_t  g_l2_saved_page    = -1;     /* page to revert to on release */

/* L3 (left stick click) edge detection for accent toggle */
static bool     g_l3_prev          = false;

/* ─── IPC Shared Memory ──────────────────────────────────────────── */

static volatile ThumbGridSharedState *g_ipc_map  = NULL;
static int                      g_ipc_fd   = -1;

#define BS_INITIAL_DELAY_US   400000   /* 400ms before first repeat */
#define BS_REPEAT_INTERVAL_US  60000   /* 60ms between repeats */

/* ─── Performance Instrumentation ─────────────────────────────────── */
static uint64_t g_perf_last_log_us    = 0;
static uint32_t g_perf_poll_count     = 0;     /* calls to GetStatus per interval */
static uint64_t g_perf_poll_total_us  = 0;     /* total time in GetStatus */
static uint64_t g_perf_poll_max_us    = 0;     /* worst-case GetStatus */
static uint64_t g_perf_render_total_us = 0;    /* time in overlay_force_draw */
static uint64_t g_perf_render_max_us  = 0;
static uint64_t g_perf_input_total_us = 0;     /* time in input processing */
#define PERF_LOG_INTERVAL_US  1000000  /* log every 1 second */

/* ─── GoldHEN Detour Hooks ────────────────────────────────────────── */

static Detour g_hook_ime_init;
static Detour g_hook_ime_status;
static Detour g_hook_ime_result;
static Detour g_hook_ime_term;

/* ─── IPC Helpers ─────────────────────────────────────────────────── */

#ifndef MAP_SHARED
#define MAP_SHARED  0x0001
#endif
#ifndef MAP_FAILED
#define MAP_FAILED  ((void *)-1)
#endif
#ifndef PROT_READ
#define PROT_READ   0x01
#endif
#ifndef PROT_WRITE
#define PROT_WRITE  0x02
#endif

static const char *g_ipc_path_used = NULL;

static bool ipc_open(void) {
    if (g_ipc_map) return true;

    /* Try multiple paths — /user/data/ first since it's shared across
     * process sandboxes (game and SceShellUI can both access it) */
    static const char *ipc_paths[] = {
        "/user/data/thumbgrid_ipc.bin",
        "/data/thumbgrid_ipc.bin",
        "/tmp/thumbgrid_ipc.bin",
        NULL
    };

    for (int i = 0; ipc_paths[i]; i++) {
        g_ipc_fd = sceKernelOpen(ipc_paths[i],
                                  0x0602, /* O_RDWR | O_CREAT | O_TRUNC */
                                  0666);
        if (g_ipc_fd >= 0) {
            g_ipc_path_used = ipc_paths[i];
            LOG_INFO("IPC: opened %s (fd=%d)", ipc_paths[i], g_ipc_fd);
            break;
        }
        LOG_DEBUG("IPC: %s failed: 0x%08X", ipc_paths[i], g_ipc_fd);
    }
    if (g_ipc_fd < 0) {
        LOG_ERROR("IPC: all paths failed");
        return false;
    }

    /* Extend file to TG_IPC_FILE_SIZE */
    char zero = 0;
    sceKernelLseek(g_ipc_fd, TG_IPC_FILE_SIZE - 1, 0 /* SEEK_SET */);
    sceKernelWrite(g_ipc_fd, &zero, 1);
    sceKernelLseek(g_ipc_fd, 0, 0);

    /* mmap shared */
    void *addr = NULL;
    int rc = sceKernelMmap(0, TG_IPC_FILE_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, g_ipc_fd, 0, &addr);
    if (rc < 0 || addr == MAP_FAILED || !addr) {
        LOG_ERROR("IPC: mmap failed: 0x%08X addr=%p", rc, addr);
        sceKernelClose(g_ipc_fd);
        g_ipc_fd = -1;
        return false;
    }

    g_ipc_map = (volatile ThumbGridSharedState *)addr;
    memset((void *)g_ipc_map, 0, sizeof(ThumbGridSharedState));
    LOG_INFO("IPC: mapped at %p (fd=%d)", addr, g_ipc_fd);
    return true;
}

static void ipc_close(void) {
    if (g_ipc_map) {
        /* Signal inactive before unmapping */
        thumbgrid_ipc_write_begin(g_ipc_map);
        ((ThumbGridSharedState *)g_ipc_map)->ime_active = 0;
        thumbgrid_ipc_write_end(g_ipc_map);

        sceKernelMunmap((void *)g_ipc_map, TG_IPC_FILE_SIZE);
        g_ipc_map = NULL;
    }
    if (g_ipc_fd >= 0) {
        sceKernelClose(g_ipc_fd);
        g_ipc_fd = -1;
    }
}

static void ipc_sync_state(void) {
    if (!g_ipc_map) return;
    if (!g_custom_active || g_session.state != IME_STATE_ACTIVE) {
        /* Just mark inactive */
        if (g_ipc_map->ime_active) {
            thumbgrid_ipc_write_begin(g_ipc_map);
            ((ThumbGridSharedState *)g_ipc_map)->ime_active = 0;
            thumbgrid_ipc_write_end(g_ipc_map);
        }
        return;
    }

    ThumbGridSharedState *m = (ThumbGridSharedState *)g_ipc_map;

    thumbgrid_ipc_write_begin(g_ipc_map);

    m->ime_active    = 1;
    m->selected_cell = g_tgrid.selected_cell;
    m->current_page  = g_tgrid.current_page;
    m->accent_mode   = g_tgrid.accent_mode ? 1 : 0;
    m->output_length = g_session.output_length;
    m->text_cursor   = g_session.text_cursor;
    m->selected_all  = g_session.selected_all ? 1 : 0;
    m->sel_start     = g_session.sel_start;
    m->sel_end       = g_session.sel_end;
    m->offset_x      = g_tgrid.offset_x;
    m->offset_y      = g_tgrid.offset_y;

    /* Copy output buffer */
    uint32_t copy_len = g_session.output_length;
    if (copy_len > TG_IPC_MAX_OUTPUT) copy_len = TG_IPC_MAX_OUTPUT;
    memcpy(m->output, g_session.output, copy_len * sizeof(uint16_t));

    /* Copy title (UTF-16) */
    memcpy(m->title, g_tgrid.title, TG_IPC_TITLE_MAX * sizeof(uint16_t));

    /* Copy page name */
    const ThumbGridPage *page = &g_tgrid.pages[g_tgrid.current_page];
    strncpy(m->page_name, page->name, TG_IPC_PAGE_NAME_MAX - 1);
    m->page_name[TG_IPC_PAGE_NAME_MAX - 1] = '\0';

    /* Copy cell characters */
    memcpy(m->cells, page->chars, sizeof(m->cells));

    /* L2+center override: show Cut/Copy/Paste/Caps on center cell */
    if (g_l2_shift_active) {
        m->cells[TG_CENTER_CELL][TG_BTN_TRIANGLE] = TG_SPECIAL_PASTE;
        m->cells[TG_CENTER_CELL][TG_BTN_CIRCLE]   = TG_SPECIAL_CAPS;
        m->cells[TG_CENTER_CELL][TG_BTN_CROSS]    = TG_SPECIAL_CUT;
        m->cells[TG_CENTER_CELL][TG_BTN_SQUARE]   = TG_SPECIAL_COPY;
    }

    m->shift_active = g_l2_shift_active ? 1 : 0;

    thumbgrid_ipc_write_end(g_ipc_map);
}

/* ─── Helper: Resolve User ID ─────────────────────────────────────── */

static int32_t ime_hook_get_user_id(int32_t param_user_id) {
    if (param_user_id > 0) {
        return param_user_id;
    }
    int32_t uid = -1;
    int32_t rc = sceUserServiceGetInitialUser(&uid);
    if (rc == 0 && uid >= 0) {
        return uid;
    }
    LOG_WARN("sceUserServiceGetInitialUser failed: 0x%08X", rc);
    return 0;  /* user 0 as last resort */
}

/* ─── Helper: Pad Open / Close ────────────────────────────────────── */

static bool ime_hook_open_pad(int32_t user_id) {
    /* Strategy 1: Get the game's existing pad handle */
    int32_t handle = scePadGetHandle(user_id, 0, 0);
    if (handle >= 0) {
        g_pad_handle = handle;
        g_owns_pad = false;
        LOG_INFO("Reusing game pad handle: %d", handle);
        return true;
    }

    /* Strategy 2: Open our own pad */
    handle = scePadOpen(user_id, 0, 0, NULL);
    if (handle >= 0) {
        g_pad_handle = handle;
        g_owns_pad = true;
        LOG_INFO("Opened new pad handle: %d", handle);
        return true;
    }

    LOG_ERROR("Failed to open pad: 0x%08X", handle);
    g_pad_handle = -1;
    g_owns_pad = false;
    return false;
}

static void ime_hook_close_pad(void) {
    if (g_pad_handle >= 0 && g_owns_pad) {
        scePadClose(g_pad_handle);
        LOG_DEBUG("Closed pad handle: %d", g_pad_handle);
    }
    g_pad_handle = -1;
    g_owns_pad = false;
}

/* ─── Overlay Draw Callback ───────────────────────────────────────── */

static void thumbgrid_draw_callback(uint32_t *fb, uint32_t pitch,
                               uint32_t width, uint32_t height)
{
    if (!g_custom_active || g_session.state != IME_STATE_ACTIVE) {
        return;
    }

    /* Cache screen dimensions for position clamping in poll loop */
    g_overlay_screen_w = width;
    g_overlay_screen_h = height;

    thumbgrid_draw(&g_tgrid, &g_session, fb, pitch, width, height);
}

/* ─── Notification Fallback Display ───────────────────────────────── */

/**
 * Display ThumbGrid state via PS4 notification when framebuffer overlay
 * is unavailable. Shows current cell characters, text buffer, and page.
 */
static void notify_fallback_display(uint64_t now_us) {
    if (g_session.state != IME_STATE_ACTIVE) return;

    /* Throttle updates */
    if (now_us - g_last_notify_time_us < IME_NOTIFY_INTERVAL_US) return;

    /* Build compact display of current cell's characters */
    const ThumbGridPage *page = &g_tgrid.pages[g_tgrid.current_page];
    int cell = g_tgrid.selected_cell;
    char c_tri = page->chars[cell][TG_BTN_TRIANGLE];
    char c_cir = page->chars[cell][TG_BTN_CIRCLE];
    char c_crs = page->chars[cell][TG_BTN_CROSS];
    char c_sqr = page->chars[cell][TG_BTN_SQUARE];

    /* Special function labels for center cell */
    char tri_buf[4], cir_buf[4], crs_buf[4], sqr_buf[4];

    #define CHAR_OR_LABEL(c, buf) do { \
        if ((c) == TG_SPECIAL_BKSP) { buf[0]='B'; buf[1]='S'; buf[2]=0; } \
        else if ((c) == TG_SPECIAL_SPACE) { buf[0]='S'; buf[1]='P'; buf[2]=0; } \
        else { buf[0]=(c); buf[1]=0; } \
    } while(0)

    CHAR_OR_LABEL(c_tri, tri_buf);
    CHAR_OR_LABEL(c_cir, cir_buf);
    CHAR_OR_LABEL(c_crs, crs_buf);
    CHAR_OR_LABEL(c_sqr, sqr_buf);
    #undef CHAR_OR_LABEL

    /* Convert text buffer */
    char text_buf[48];
    uint32_t tlen = g_session.output_length;
    if (tlen > 40) tlen = 40;
    for (uint32_t i = 0; i < tlen; i++) {
        uint16_t ch = g_session.output[i];
        text_buf[i] = (ch < 128) ? (char)ch : '?';
    }
    text_buf[tlen] = '_';
    text_buf[tlen + 1] = '\0';

    /* Simple hash to avoid redundant updates */
    uint32_t hash = (uint32_t)cell ^ (tlen << 8) ^
                    ((uint32_t)g_tgrid.current_page << 16) ^
                    ((uint32_t)c_tri << 24);
    if (hash == g_last_display_hash) return;
    g_last_display_hash = hash;
    g_last_notify_time_us = now_us;

    /* Format:
     * [abc] Cell 3
     * /\=m O=n X=o []=p
     * >Hello world_
     */
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type     = NotificationRequest;
    req.reqId    = IME_NOTIFY_REQ_ID;
    req.priority = 0;
    req.msgId    = 0;
    req.targetId = -1;
    req.userId   = -1;
    snprintf(req.message, sizeof(req.message),
        "[%s] Cell %d\n"
        "/\\=%s O=%s X=%s []=%s\n"
        ">%s",
        page->name, cell,
        tri_buf, cir_buf, crs_buf, sqr_buf,
        text_buf);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ─── ThumbGrid Action Dispatch ────────────────────────────────────── */

static void dispatch_face_button(int button_index) {
    char ch = thumbgrid_get_char(&g_tgrid, button_index);
    if (ch == 0) return;

    if (thumbgrid_is_special(&g_tgrid, button_index)) {
        /* Center cell special functions */
        switch (ch) {
        case TG_SPECIAL_BKSP:
            ime_session_backspace(&g_session);
            break;
        case TG_SPECIAL_SPACE:
            ime_session_add_char(&g_session, ' ');
            break;
        case TG_SPECIAL_ACCENT:
            thumbgrid_toggle_accent(&g_tgrid);
            LOG_DEBUG("ThumbGrid: accent mode %s", g_tgrid.accent_mode ? "ON" : "OFF");
            break;
        case TG_SPECIAL_SELALL:
            ime_session_select_all(&g_session);
            LOG_DEBUG("ThumbGrid: select all");
            break;
        case TG_SPECIAL_EXIT:
            ime_session_cancel(&g_session);
            LOG_INFO("ThumbGrid: exit via center cell");
            break;
        }
    } else {
        /* Normal character — apply accent if active */
        if (g_tgrid.accent_mode) {
            uint16_t accented = thumbgrid_accent_lookup(ch);
            if (accented) {
                ime_session_add_char16(&g_session, accented);
                return;
            }
        }
        ime_session_add_char(&g_session, ch);
    }
}

/* ─── Hooked Functions ────────────────────────────────────────────── */

static int32_t hooked_ime_dialog_init(const OrbisImeDialogParam *param,
                                      void *param_extended) {
    if (!param) {
        LOG_ERROR("sceImeDialogInit called with NULL param");
        if (g_hook_state.original_init) {
            return g_hook_state.original_init(param, param_extended);
        }
        return -1;
    }

    LOG_INFO(">>> sceImeDialogInit intercepted");
    LOG_DEBUG("  user_id:    %d", param->user_id);
    LOG_DEBUG("  type:       %d", param->type);
    LOG_DEBUG("  max_length: %u", param->max_text_length);
    LOG_DEBUG("  option:     0x%08X", param->option);
    LOG_DEBUG("  input_buf:  %p", (void *)param->input_text_buffer);

    /* Default max_text_length if game sends 0 */
    uint32_t max_len = param->max_text_length;
    if (max_len == 0) {
        max_len = IME_DEFAULT_MAX_LENGTH;
    }

    /* Resolve user ID */
    g_user_id = ime_hook_get_user_id(param->user_id);
    LOG_DEBUG("  resolved user_id: %d", g_user_id);

    /* Initialize the IME session */
    int32_t rc = ime_session_init(&g_session, param->type,
        max_len, param->input_text_buffer, param->input_text_buffer);
    if (rc != IME_OK) {
        LOG_ERROR("ime_session_init failed: %d, falling back to system IME", rc);
        g_custom_active = false;
        if (g_hook_state.original_init) {
            return g_hook_state.original_init(param, param_extended);
        }
        return rc;
    }

    /* Open pad handle */
    if (!ime_hook_open_pad(g_user_id)) {
        LOG_ERROR("Failed to open pad, falling back to system IME");
        g_custom_active = false;
        g_session.state = IME_STATE_INACTIVE;
        if (g_hook_state.original_init) {
            return g_hook_state.original_init(param, param_extended);
        }
        return IME_ERROR_GENERIC;
    }

    /* Initialize ThumbGrid grid state */
    thumbgrid_init(&g_tgrid);

    /* Open IPC shared memory for shell overlay */
    ipc_open();

    /* Capture title from IME param (keep as UTF-16) */
    g_tgrid.title[0] = 0;
    if (param->title) {
        const uint16_t *t = param->title;
        uint32_t i = 0;
        while (t[i] != 0 && i < TG_TITLE_MAX - 1) {
            g_tgrid.title[i] = t[i];
            i++;
        }
        g_tgrid.title[i] = 0;
        LOG_DEBUG("  title: (UTF-16, %u chars)", i);
    }

    /* Reset input state and start grace period */
    memset(&g_input_state, 0, sizeof(g_input_state));
    g_input_state.stick_x  = 128;  /* center */
    g_input_state.stick_y  = 128;
    g_input_state.rstick_x = 128;
    g_input_state.rstick_y = 128;
    g_session_start_us    = sceKernelGetProcessTime();
    g_last_notify_time_us = 0;
    g_last_display_hash   = 0;

    g_custom_active = true;

    /* Reset repeat state */
    g_bs_held = false;

    /* Reset X hold and L2 analog state */
    g_x_held = false;
    g_x_dpad_used = false;
    g_l2_prev_analog = 0;
    g_l2_shift_active = false;
    g_l2_saved_page = -1;
    g_l3_prev = false;

    /* Framebuffer overlay disabled — PUI shell overlay handles rendering */
    /* overlay_set_draw_callback(thumbgrid_draw_callback); */

    LOG_INFO("ThumbGrid IME session started (max=%u)", max_len);

    /* Return OK — do NOT call original (no system keyboard) */
    return IME_OK;
}

static OrbisImeDialogStatus hooked_ime_dialog_get_status(void) {
    /* Pass through if not in custom mode */
    if (!g_custom_active) {
        if (!g_hook_state.original_get_status) {
            return ORBIS_IME_DIALOG_STATUS_NONE;
        }
        return g_hook_state.original_get_status();
    }

    /* Map terminal states immediately */
    if (g_session.state == IME_STATE_CONFIRMING ||
        g_session.state == IME_STATE_CANCELLED) {
        return ORBIS_IME_DIALOG_STATUS_FINISHED;
    }
    if (g_session.state == IME_STATE_INACTIVE) {
        return ORBIS_IME_DIALOG_STATUS_NONE;
    }

    /* === Active state: process input === */

    /* 1. Get timestamp */
    uint64_t now_us = sceKernelGetProcessTime();
    uint64_t poll_entry_us = now_us;  /* PERF: entry time */

    /* 2. Read controller */
    OrbisPadData pad_data;
    memset(&pad_data, 0, sizeof(pad_data));

    if (g_pad_handle >= 0) {
        int32_t rc = scePadReadState(g_pad_handle, &pad_data);
        if (rc != 0) {
            LOG_DEBUG("scePadReadState failed: 0x%08X", rc);
        }
    }

    /* 3. Update input edge detection (always, to keep state current) */
    input_update(&g_input_state, pad_data.buttons,
                 pad_data.leftStick.x, pad_data.leftStick.y,
                 pad_data.rightStick.x, pad_data.rightStick.y, now_us);

    /* 4. Update cell selection from left analog stick */
    thumbgrid_select_cell(&g_tgrid, g_input_state.stick_x, g_input_state.stick_y);

    /* 4b. Update widget position from right analog stick */
    thumbgrid_update_position(&g_tgrid, g_input_state.rstick_x, g_input_state.rstick_y,
                         g_overlay_screen_w, g_overlay_screen_h);

    uint64_t input_done_us = sceKernelGetProcessTime();  /* PERF */
    g_perf_input_total_us += (input_done_us - poll_entry_us);

    /*
     * 5. Grace period: ignore all actions for the first 300ms.
     * The player is likely still holding whatever button opened the
     * text field. We keep reading the pad (step 3) so edge detection
     * stays accurate, but don't act on anything until the grace
     * period expires.
     */
    if (now_us - g_session_start_us < IME_GRACE_PERIOD_US) {
        /* Keep L2/L3 tracking in sync during grace so we don't
         * get a false trigger on the first post-grace frame */
        g_l2_prev_analog = pad_data.analogButtons.l2;
        g_l3_prev = (pad_data.buttons & PAD_BUTTON_L3) != 0;
        /* Show fallback display during grace period too */
        if (!overlay_is_active()) {
            notify_fallback_display(now_us);
        }
        return ORBIS_IME_DIALOG_STATUS_RUNNING;
    }

    /* 6. L2 analog trigger: hold for shift */
    {
        uint8_t l2 = pad_data.analogButtons.l2;

        /* L2 just crossed engage threshold → apply temporary shift */
        if (l2 >= 60 && !g_l2_shift_active && g_l2_prev_analog < 60) {
            g_l2_saved_page = g_tgrid.current_page;
            /* Toggle between page 0 (lower) and 1 (upper) */
            if (g_tgrid.current_page == 0)      g_tgrid.current_page = 1;
            else if (g_tgrid.current_page == 1)  g_tgrid.current_page = 0;
            g_l2_shift_active = true;
        }

        /* L2 released → revert temporary shift */
        if (l2 < 40 && g_l2_prev_analog >= 40) {
            if (g_l2_shift_active && g_l2_saved_page >= 0) {
                g_tgrid.current_page = g_l2_saved_page;
            }
            g_l2_shift_active = false;
            g_l2_saved_page = -1;
        }

        g_l2_prev_analog = l2;
    }

    /* 6a. L3 (left stick click): accent toggle */
    {
        bool l3_now = (pad_data.buttons & PAD_BUTTON_L3) != 0;
        if (l3_now && !g_l3_prev) {
            thumbgrid_toggle_accent(&g_tgrid);
            LOG_DEBUG("ThumbGrid: L3 accent mode %s", g_tgrid.accent_mode ? "ON" : "OFF");
        }
        g_l3_prev = l3_now;
    }

    /* 6b. L2+center override: Cut/Copy/Paste/Caps on center cell while shift held */
    bool l2_center_override = (g_l2_shift_active && g_tgrid.selected_cell == TG_CENTER_CELL);

    /* 6c. X (cross) hold state machine for text selection */
    {
        bool x_pressed = input_just_pressed(&g_input_state, PAD_BUTTON_CROSS);
        bool x_held_now = input_is_held(&g_input_state, PAD_BUTTON_CROSS);
        bool x_released = (g_input_state.buttons_released & PAD_BUTTON_CROSS) != 0;

        if (x_pressed) {
            g_x_held = true;
            g_x_dpad_used = false;
            g_x_anchor = g_session.text_cursor;
        }

        if (x_released && g_x_held) {
            if (!g_x_dpad_used) {
                if (l2_center_override) {
                    /* L2+center X → Cut */
                    ime_session_cut(&g_session);
                    LOG_DEBUG("ThumbGrid: L2+center X = cut");
                } else {
                    /* X tapped without D-pad → input character */
                    dispatch_face_button(TG_BTN_CROSS);
                }
            }
            g_x_held = false;
            /* Keep selection active if D-pad was used */
        }

        (void)x_held_now; /* used implicitly via g_x_held */
    }

    /* 7. Get action from button edges */
    ImeAction action = input_get_action(&g_input_state);

    /* 7a. Dispatch action */
    switch (action) {
    case IME_ACTION_CANCEL:
        ime_session_cancel(&g_session);
        LOG_INFO("ThumbGrid: cancelled");
        return ORBIS_IME_DIALOG_STATUS_FINISHED;

    case IME_ACTION_SUBMIT:
        ime_session_submit(&g_session);
        LOG_INFO("ThumbGrid: R2 submit (%u chars)", g_session.output_length);
        return ORBIS_IME_DIALOG_STATUS_FINISHED;

    case IME_ACTION_FACE_TRIANGLE:
        if (l2_center_override) {
            ime_session_paste(&g_session);
            LOG_DEBUG("ThumbGrid: L2+center Triangle = paste");
        } else {
            dispatch_face_button(TG_BTN_TRIANGLE);
        }
        break;

    case IME_ACTION_FACE_CIRCLE:
        if (l2_center_override) {
            /* Caps lock toggle: make current page permanent */
            g_l2_shift_active = false;
            g_l2_saved_page = -1;  /* don't revert on L2 release */
            LOG_DEBUG("ThumbGrid: L2+center Circle = caps lock -> page %d", g_tgrid.current_page);
        } else {
            dispatch_face_button(TG_BTN_CIRCLE);
        }
        break;

    case IME_ACTION_FACE_SQUARE:
        if (l2_center_override) {
            ime_session_copy(&g_session);
            LOG_DEBUG("ThumbGrid: L2+center Square = copy");
        } else {
            dispatch_face_button(TG_BTN_SQUARE);
        }
        break;

    case IME_ACTION_CURSOR_HOME:
        if (g_x_held) {
            /* X held + D-pad up: select from anchor to start */
            g_x_dpad_used = true;
            g_session.text_cursor = 0;
            ime_session_set_selection(&g_session, g_x_anchor, 0);
        } else {
            ime_session_clear_selection(&g_session);
            ime_session_cursor_home(&g_session);
        }
        break;

    case IME_ACTION_CURSOR_END:
        if (g_x_held) {
            /* X held + D-pad down: select from anchor to end */
            g_x_dpad_used = true;
            g_session.text_cursor = g_session.output_length;
            ime_session_set_selection(&g_session, g_x_anchor,
                                      g_session.output_length);
        } else {
            ime_session_clear_selection(&g_session);
            ime_session_cursor_end(&g_session);
        }
        break;

    case IME_ACTION_CURSOR_LEFT:
        if (g_x_held) {
            g_x_dpad_used = true;
            if (g_session.text_cursor > 0) g_session.text_cursor--;
            ime_session_set_selection(&g_session, g_x_anchor,
                                      g_session.text_cursor);
        } else {
            ime_session_clear_selection(&g_session);
            ime_session_cursor_left(&g_session);
        }
        break;

    case IME_ACTION_CURSOR_RIGHT:
        if (g_x_held) {
            g_x_dpad_used = true;
            if (g_session.text_cursor < g_session.output_length)
                g_session.text_cursor++;
            ime_session_set_selection(&g_session, g_x_anchor,
                                      g_session.text_cursor);
        } else {
            ime_session_clear_selection(&g_session);
            ime_session_cursor_right(&g_session);
        }
        break;

    case IME_ACTION_PAGE_NEXT:
    case IME_ACTION_PAGE_PREV:
        thumbgrid_toggle_symbols(&g_tgrid);
        LOG_DEBUG("ThumbGrid: L1/R1 symbols -> page %d", g_tgrid.current_page);
        break;

    case IME_ACTION_NONE:
    default:
        break;
    }

    /* 7b. Backspace hold-to-repeat */
    {
        bool sq_held = input_is_held(&g_input_state, PAD_BUTTON_SQUARE);
        char sq_char = thumbgrid_get_char(&g_tgrid, TG_BTN_SQUARE);
        bool is_bs = (sq_char == TG_SPECIAL_BKSP);

        if (sq_held && is_bs) {
            if (!g_bs_held) {
                g_bs_held = true;
                g_bs_start_us = now_us;
                g_bs_last_repeat_us = now_us;
            } else if (now_us - g_bs_start_us >= BS_INITIAL_DELAY_US) {
                if (now_us - g_bs_last_repeat_us >= BS_REPEAT_INTERVAL_US) {
                    ime_session_backspace(&g_session);
                    g_bs_last_repeat_us = now_us;
                }
            }
        } else {
            g_bs_held = false;
        }
    }

    /* 7c. Sync state to IPC shared memory for shell overlay */
    ipc_sync_state();

    /* PERF: Accumulate poll stats and log once per second */
    {
        uint64_t poll_exit_us = sceKernelGetProcessTime();
        uint64_t poll_us = poll_exit_us - poll_entry_us;
        g_perf_poll_count++;
        g_perf_poll_total_us += poll_us;
        if (poll_us > g_perf_poll_max_us) g_perf_poll_max_us = poll_us;

        if (poll_exit_us - g_perf_last_log_us >= PERF_LOG_INTERVAL_US) {
            printf("[CIME] PERF: polls/s=%u  avg=%luus  max=%luus | "
                   "render avg=%luus max=%luus | input avg=%luus\n",
                g_perf_poll_count,
                (unsigned long)(g_perf_poll_count ? g_perf_poll_total_us / g_perf_poll_count : 0),
                (unsigned long)g_perf_poll_max_us,
                (unsigned long)(g_perf_poll_count ? g_perf_render_total_us / g_perf_poll_count : 0),
                (unsigned long)g_perf_render_max_us,
                (unsigned long)(g_perf_poll_count ? g_perf_input_total_us / g_perf_poll_count : 0));
            g_perf_last_log_us     = poll_exit_us;
            g_perf_poll_count      = 0;
            g_perf_poll_total_us   = 0;
            g_perf_poll_max_us     = 0;
            g_perf_render_total_us = 0;
            g_perf_render_max_us   = 0;
            g_perf_input_total_us  = 0;
        }
    }

    return ORBIS_IME_DIALOG_STATUS_RUNNING;
}

static int32_t hooked_ime_dialog_get_result(OrbisImeDialogResult *result) {
    if (!g_custom_active) {
        if (!g_hook_state.original_get_result) {
            return IME_ERROR_NOT_INITIALIZED;
        }
        int32_t rc = g_hook_state.original_get_result(result);
        if (result) {
            LOG_DEBUG("sceImeDialogGetResult: end_status=%d",
                result->end_status);
        }
        return rc;
    }

    if (!result) {
        return IME_ERROR_INVALID_PARAM;
    }

    memset(result, 0, sizeof(OrbisImeDialogResult));

    if (g_session.state == IME_STATE_CONFIRMING) {
        result->end_status = ORBIS_IME_DIALOG_END_STATUS_OK;
        LOG_DEBUG("GetResult: OK (text already in caller buffer)");
    } else if (g_session.state == IME_STATE_CANCELLED) {
        result->end_status = ORBIS_IME_DIALOG_END_STATUS_USER_CANCELED;
        LOG_DEBUG("GetResult: USER_CANCELED");
    } else {
        result->end_status = ORBIS_IME_DIALOG_END_STATUS_ABORTED;
        LOG_WARN("GetResult: unexpected state %d", g_session.state);
    }

    return IME_OK;
}

static int32_t hooked_ime_dialog_term(void) {
    LOG_DEBUG("sceImeDialogTerm called");

    if (g_custom_active) {
        /* Disable overlay rendering */
        overlay_set_draw_callback(NULL);

        /* Signal shell overlay to hide grid */
        if (g_ipc_map) {
            thumbgrid_ipc_write_begin(g_ipc_map);
            ((ThumbGridSharedState *)g_ipc_map)->ime_active = 0;
            thumbgrid_ipc_write_end(g_ipc_map);
        }

        ime_hook_close_pad();
        g_custom_active = false;
        g_session.state = IME_STATE_INACTIVE;
        memset(&g_input_state, 0, sizeof(g_input_state));
        g_last_notify_time_us = 0;
        g_last_display_hash   = 0;
        LOG_INFO("ThumbGrid IME session terminated");
        return IME_OK;
    }

    if (!g_hook_state.original_term) {
        return IME_ERROR_NOT_INITIALIZED;
    }
    return g_hook_state.original_term();
}

/* ─── Hook Installation ───────────────────────────────────────────── */

int32_t ime_hook_install(void) {
    if (g_hook_state.hooks_installed) {
        LOG_WARN("Hooks already installed");
        return IME_OK;
    }

    LOG_INFO("Installing IME dialog hooks...");

    memset(&g_hook_state, 0, sizeof(g_hook_state));
    memset(&g_session, 0, sizeof(g_session));
    memset(&g_input_state, 0, sizeof(g_input_state));

    /*
     * Load the IME Dialog module and resolve function addresses.
     * sceKernelLoadStartModule returns a handle if already loaded,
     * or loads it fresh.
     */
    int mod_handle = sceKernelLoadStartModule(
        "libSceImeDialog.sprx", 0, NULL, 0, NULL, NULL);

    if (mod_handle < 0) {
        LOG_WARN("libSceImeDialog.sprx not available yet: 0x%08X", mod_handle);
        LOG_INFO("Hooks deferred - module will be loaded by game");
        g_hook_state.initialized = true;
        return IME_OK;
    }

    LOG_INFO("libSceImeDialog.sprx handle: 0x%08X", mod_handle);

    /* Resolve function addresses by symbol name */
    void *addr_init   = NULL;
    void *addr_status = NULL;
    void *addr_result = NULL;
    void *addr_term   = NULL;

    sceKernelDlsym(mod_handle, "sceImeDialogInit", &addr_init);
    sceKernelDlsym(mod_handle, "sceImeDialogGetStatus", &addr_status);
    sceKernelDlsym(mod_handle, "sceImeDialogGetResult", &addr_result);
    sceKernelDlsym(mod_handle, "sceImeDialogTerm", &addr_term);

    LOG_DEBUG("Resolved: Init=%p Status=%p Result=%p Term=%p",
        addr_init, addr_status, addr_result, addr_term);

    /* Install hooks for resolved functions */
    if (addr_init) {
        Detour_Construct(&g_hook_ime_init, DetourMode_x64);
        Detour_DetourFunction(&g_hook_ime_init, (uint64_t)addr_init, hooked_ime_dialog_init);
        g_hook_state.original_init =
            (sceImeDialogInit_t)g_hook_ime_init.StubPtr;
        LOG_INFO("Hooked sceImeDialogInit @ %p", addr_init);
    }

    if (addr_status) {
        Detour_Construct(&g_hook_ime_status, DetourMode_x64);
        Detour_DetourFunction(&g_hook_ime_status, (uint64_t)addr_status, hooked_ime_dialog_get_status);
        g_hook_state.original_get_status =
            (sceImeDialogGetStatus_t)g_hook_ime_status.StubPtr;
        LOG_INFO("Hooked sceImeDialogGetStatus @ %p", addr_status);
    }

    if (addr_result) {
        Detour_Construct(&g_hook_ime_result, DetourMode_x64);
        Detour_DetourFunction(&g_hook_ime_result, (uint64_t)addr_result, hooked_ime_dialog_get_result);
        g_hook_state.original_get_result =
            (sceImeDialogGetResult_t)g_hook_ime_result.StubPtr;
        LOG_INFO("Hooked sceImeDialogGetResult @ %p", addr_result);
    }

    if (addr_term) {
        Detour_Construct(&g_hook_ime_term, DetourMode_x64);
        Detour_DetourFunction(&g_hook_ime_term, (uint64_t)addr_term, hooked_ime_dialog_term);
        g_hook_state.original_term =
            (sceImeDialogTerm_t)g_hook_ime_term.StubPtr;
        LOG_INFO("Hooked sceImeDialogTerm @ %p", addr_term);
    }

    g_hook_state.hooks_installed = (addr_init != NULL);
    g_hook_state.initialized = true;

    LOG_INFO("Hook installation complete (installed=%s)",
        g_hook_state.hooks_installed ? "YES" : "NO - deferred");

    return IME_OK;
}

int32_t ime_hook_remove(void) {
    if (!g_hook_state.initialized) {
        return IME_OK;
    }

    LOG_INFO("Removing IME hooks...");

    /* Clean up any active session */
    if (g_custom_active) {
        overlay_set_draw_callback(NULL);
        ime_hook_close_pad();
        g_custom_active = false;
    }

    /* Close IPC shared memory */
    ipc_close();

    if (g_hook_state.hooks_installed) {
        if (g_hook_state.original_init) {
            Detour_RestoreFunction(&g_hook_ime_init);
            Detour_Destroy(&g_hook_ime_init);
        }
        if (g_hook_state.original_get_status) {
            Detour_RestoreFunction(&g_hook_ime_status);
            Detour_Destroy(&g_hook_ime_status);
        }
        if (g_hook_state.original_get_result) {
            Detour_RestoreFunction(&g_hook_ime_result);
            Detour_Destroy(&g_hook_ime_result);
        }
        if (g_hook_state.original_term) {
            Detour_RestoreFunction(&g_hook_ime_term);
            Detour_Destroy(&g_hook_ime_term);
        }
    }

    memset(&g_hook_state, 0, sizeof(g_hook_state));
    memset(&g_input_state, 0, sizeof(g_input_state));
    g_custom_active = false;
    g_session.state = IME_STATE_INACTIVE;

    LOG_INFO("All hooks removed");
    return IME_OK;
}

const ImeHookState *ime_hook_get_state(void) {
    return &g_hook_state;
}
