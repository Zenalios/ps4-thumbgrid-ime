/**
 * @file thumbgrid.c
 * @brief ThumbGrid 3x3 grid text input engine — character maps, cell selection, rendering
 *
 * PS4-style button mapping: all 4 face buttons input characters on non-center cells.
 * Center cell (4): Triangle=Space, Circle=Exit, Cross=SelectAll, Square=Backspace.
 * R2=submit, L2=shift hold, L1/R1=letters/symbols, D-pad=text cursor.
 */

#include <string.h>

#include "plugin_common.h"
#include "thumbgrid.h"
#include "ime_custom.h"
#include "overlay.h"

#include <orbis/libkernel.h>

/* ─── Character Pages ────────────────────────────────────────────── */

/*
 * Cell layout (analog stick positions):
 *   0(UL)  1(UC)  2(UR)     ← stick up
 *   3(ML)  4(MC)  5(MR)     ← stick center
 *   6(BL)  7(BC)  8(BR)     ← stick down
 *
 * Button order per cell: [triangle, circle, cross, square]
 *
 * Center cell (4):
 *   triangle=Space, circle=Exit IME, cross=select all, square=Backspace
 */

#define SPC TG_SPECIAL_SPACE
#define BKS TG_SPECIAL_BKSP
#define ACC TG_SPECIAL_ACCENT
#define SEL TG_SPECIAL_SELALL
#define EXT TG_SPECIAL_EXIT

static const ThumbGridPage g_thumbgrid_pages[TG_MAX_PAGES] = {
    /* Page 0: lowercase */
    {
        .name = "abc",
        .chars = {
            /* Cell 0 (UL) */ {'a', 'b', 'c', 'd'},
            /* Cell 1 (UC) */ {'e', 'f', 'g', 'h'},
            /* Cell 2 (UR) */ {'i', 'j', 'k', 'l'},
            /* Cell 3 (ML) */ {'m', 'n', 'o', 'p'},
            /* Cell 4 (MC) */ {SPC, EXT, SEL, BKS},
            /* Cell 5 (MR) */ {'q', 'r', 's', 't'},
            /* Cell 6 (BL) */ {'u', 'v', 'w', 'x'},
            /* Cell 7 (BC) */ {'y', 'z', '.', ','},
            /* Cell 8 (BR) */ {'!', '?', '\'','-'},
        },
    },
    /* Page 1: UPPERCASE */
    {
        .name = "ABC",
        .chars = {
            /* Cell 0 (UL) */ {'A', 'B', 'C', 'D'},
            /* Cell 1 (UC) */ {'E', 'F', 'G', 'H'},
            /* Cell 2 (UR) */ {'I', 'J', 'K', 'L'},
            /* Cell 3 (ML) */ {'M', 'N', 'O', 'P'},
            /* Cell 4 (MC) */ {SPC, EXT, SEL, BKS},
            /* Cell 5 (MR) */ {'Q', 'R', 'S', 'T'},
            /* Cell 6 (BL) */ {'U', 'V', 'W', 'X'},
            /* Cell 7 (BC) */ {'Y', 'Z', '.', ','},
            /* Cell 8 (BR) */ {'!', '?', '\'','-'},
        },
    },
    /* Page 2: Numbers/Symbols */
    {
        .name = "123",
        .chars = {
            /* Cell 0 (UL) */ {'1', '2', '3', '+'},
            /* Cell 1 (UC) */ {'4', '5', '6', '='},
            /* Cell 2 (UR) */ {'7', '8', '9', '0'},
            /* Cell 3 (ML) */ {'@', '#', '$', '%'},
            /* Cell 4 (MC) */ {SPC, EXT, SEL, BKS},
            /* Cell 5 (MR) */ {'&', '*', '(', ')'},
            /* Cell 6 (BL) */ {'_', '/', '\\','|'},
            /* Cell 7 (BC) */ {'[', ']', '{', '}'},
            /* Cell 8 (BR) */ {'<', '>', '"', '~'},
        },
    },
};

#undef SPC
#undef BKS
#undef ACC
#undef SEL
#undef EXT

/* ─── Core Functions ─────────────────────────────────────────────── */

void thumbgrid_init(ThumbGridState *state) {
    if (!state) return;
    state->selected_cell = TG_CENTER_CELL;
    state->current_page  = 0;
    state->page_count    = TG_MAX_PAGES;
    state->pages         = g_thumbgrid_pages;
    state->offset_x      = 0;
    state->offset_y      = 0;
    state->accent_mode   = false;
    state->title[0]      = '\0';
}

void thumbgrid_select_cell(ThumbGridState *state, uint8_t stick_x, uint8_t stick_y) {
    if (!state) return;

    int col, row;

    if (stick_x < 78)       col = 0;
    else if (stick_x > 178) col = 2;
    else                    col = 1;

    if (stick_y < 78)       row = 0;
    else if (stick_y > 178) row = 2;
    else                    row = 1;

    state->selected_cell = row * 3 + col;
}

char thumbgrid_get_char(const ThumbGridState *state, int button_index) {
    if (!state || !state->pages) return 0;
    if (button_index < 0 || button_index >= TG_BUTTONS) return 0;
    if (state->selected_cell < 0 || state->selected_cell >= TG_CELLS) return 0;
    if (state->current_page < 0 || state->current_page >= state->page_count) return 0;

    return state->pages[state->current_page]
               .chars[state->selected_cell][button_index];
}

bool thumbgrid_is_special(const ThumbGridState *state, int button_index) {
    char c = thumbgrid_get_char(state, button_index);
    return (c == TG_SPECIAL_SPACE  || c == TG_SPECIAL_BKSP   ||
            c == TG_SPECIAL_ACCENT || c == TG_SPECIAL_SELALL ||
            c == TG_SPECIAL_EXIT   || c == TG_SPECIAL_CUT    ||
            c == TG_SPECIAL_COPY   || c == TG_SPECIAL_PASTE  ||
            c == TG_SPECIAL_CAPS);
}

void thumbgrid_shift_toggle(ThumbGridState *state) {
    if (!state) return;
    if (state->current_page == 0)
        state->current_page = 1;
    else if (state->current_page == 1)
        state->current_page = 0;
    /* page 2 stays on page 2 — use L1/R1 to leave it */
}

void thumbgrid_toggle_symbols(ThumbGridState *state) {
    if (!state) return;
    /* Toggle between letter pages (0/1) and symbols page (2) */
    if (state->current_page == 2)
        state->current_page = 0;
    else
        state->current_page = 2;
}

void thumbgrid_toggle_accent(ThumbGridState *state) {
    if (!state) return;
    state->accent_mode = !state->accent_mode;
}

/* Accent lookup: base char → accented UTF-16 code point, 0 if none */
uint16_t thumbgrid_accent_lookup(char base) {
    switch (base) {
    case 'a': return 0x00E1; /* á */
    case 'e': return 0x00E9; /* é */
    case 'i': return 0x00ED; /* í */
    case 'o': return 0x00F3; /* ó */
    case 'u': return 0x00FA; /* ú */
    case 'n': return 0x00F1; /* ñ */
    case 'A': return 0x00C1; /* Á */
    case 'E': return 0x00C9; /* É */
    case 'I': return 0x00CD; /* Í */
    case 'O': return 0x00D3; /* Ó */
    case 'U': return 0x00DA; /* Ú */
    case 'N': return 0x00D1; /* Ñ */
    default:  return 0;
    }
}

/* ─── Layout Constants ───────────────────────────────────────────── */

/* PS4 dark theme colors */
#define COL_BG_DIM      OVERLAY_COLOR(58, 58, 58)
#define COL_BG_SEL      OVERLAY_COLOR(58, 58, 58)
#define COL_BORDER      OVERLAY_COLOR(30, 30, 30)
#define COL_BORDER_SEL  OVERLAY_COLOR(200, 200, 200)
#define COL_TEXT         OVERLAY_COLOR(200, 200, 200)
#define COL_TEXT_HI      OVERLAY_COLOR(255, 255, 255)
#define COL_TEXT_SPECIAL OVERLAY_COLOR(0, 186, 177)
#define COL_TEXT_BUF     OVERLAY_COLOR(255, 255, 255)
#define COL_BG_BAR       OVERLAY_COLOR(20, 20, 20)
#define COL_CURSOR       OVERLAY_COLOR(0, 186, 177)
#define COL_TITLE        OVERLAY_COLOR(160, 160, 160)
#define COL_SELECT_BG    OVERLAY_COLOR(40, 80, 120)    /* blue tint for selection */

/* Layout — PS4-keyboard-sized (2x font = 16×16 chars) */
#define CELL_W    200
#define CELL_H    110
#define GRID_W    (CELL_W * 3 + 4)    /* 604 */
#define GRID_H    (CELL_H * 3 + 4)    /* 334 */
#define TITLE_BAR_H 28
#define TEXT_BAR_H  40
#define PAGE_BAR_H  26
#define OVL_TOTAL_W  (GRID_W + 16)    /* 620 */
#define OVL_TOTAL_H  (TITLE_BAR_H + TEXT_BAR_H + 2 + GRID_H + 2 + PAGE_BAR_H + 8)  /* 440 */

/* ─── Right Stick Widget Movement ────────────────────────────────── */

static int stick_speed(uint8_t val) {
    if (val < 40)  return -10;
    if (val < 108) return -4;
    if (val <= 148) return 0;
    if (val <= 216) return 4;
    return 10;
}

void thumbgrid_update_position(ThumbGridState *state, uint8_t rstick_x, uint8_t rstick_y,
                          uint32_t screen_w, uint32_t screen_h)
{
    if (!state) return;

    int dx = stick_speed(rstick_x);
    int dy = stick_speed(rstick_y);
    if (dx == 0 && dy == 0) return;

    state->offset_x += dx;
    state->offset_y += dy;

    /* Compute default position and clamp so overlay stays on screen */
    int def_x = ((int)screen_w - OVL_TOTAL_W) / 2;
    int def_y = (int)screen_h * 2 / 3 - OVL_TOTAL_H / 2;
    int bx = def_x + state->offset_x;
    int by = def_y + state->offset_y;

    int margin = 10;
    if (bx < margin)
        state->offset_x = margin - def_x;
    if (bx > (int)screen_w - OVL_TOTAL_W - margin)
        state->offset_x = (int)screen_w - OVL_TOTAL_W - margin - def_x;
    if (by < margin)
        state->offset_y = margin - def_y;
    if (by > (int)screen_h - OVL_TOTAL_H - margin)
        state->offset_y = (int)screen_h - OVL_TOTAL_H - margin - def_y;
}

/* ─── Rendering Helpers ──────────────────────────────────────────── */

/* Helper: draw a 2px border around a rectangle (visible at larger cell size) */
static void draw_cell_border(uint32_t *fb, uint32_t pitch,
                              int x, int y, int w, int h, uint32_t color)
{
    overlay_draw_rect(fb, pitch, x, y, w, 2, color);
    overlay_draw_rect(fb, pitch, x, y + h - 2, w, 2, color);
    overlay_draw_rect(fb, pitch, x, y, 2, h, color);
    overlay_draw_rect(fb, pitch, x + w - 2, y, 2, h, color);
}

/* Helper: get display string for a special function char */
static const char *special_label(char c) {
    switch (c) {
    case TG_SPECIAL_BKSP:   return "Del";
    case TG_SPECIAL_SPACE:  return "Space";
    case TG_SPECIAL_ACCENT: return "AC";
    case TG_SPECIAL_SELALL: return "Select";
    case TG_SPECIAL_EXIT:   return "Exit";
    case TG_SPECIAL_CUT:    return "Cut";
    case TG_SPECIAL_COPY:   return "Copy";
    case TG_SPECIAL_PASTE:  return "Paste";
    case TG_SPECIAL_CAPS:   return "CAPS";
    default:                 return "?";
    }
}

/* Helper: check if a character has an accent variant */
static bool is_accentable(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'n' ||
           c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'N';
}

/* Helper: draw accent mark (´) above a 2x character position */
static void draw_accent_mark_2x(uint32_t *fb, uint32_t pitch,
                                 int px, int py, uint32_t color)
{
    (void)pitch;
    /* Acute accent: diagonal 2px-wide line above the 16×16 glyph */
    overlay_put_pixel_ext(fb, px + 11, py - 3, color);
    overlay_put_pixel_ext(fb, px + 12, py - 3, color);
    overlay_put_pixel_ext(fb, px + 9,  py - 2, color);
    overlay_put_pixel_ext(fb, px + 10, py - 2, color);
    overlay_put_pixel_ext(fb, px + 7,  py - 1, color);
    overlay_put_pixel_ext(fb, px + 8,  py - 1, color);
}

/* Helper: map UTF-16 accented code point to ASCII base letter */
static char u16_to_base(uint16_t ch) {
    if (ch < 128) return (char)ch;
    switch (ch) {
    case 0x00C1: case 0x00C0: case 0x00C2: case 0x00C3: case 0x00C4: return 'A';
    case 0x00E1: case 0x00E0: case 0x00E2: case 0x00E3: case 0x00E4: return 'a';
    case 0x00C9: case 0x00C8: case 0x00CA: case 0x00CB: return 'E';
    case 0x00E9: case 0x00E8: case 0x00EA: case 0x00EB: return 'e';
    case 0x00CD: case 0x00CC: case 0x00CE: case 0x00CF: return 'I';
    case 0x00ED: case 0x00EC: case 0x00EE: case 0x00EF: return 'i';
    case 0x00D3: case 0x00D2: case 0x00D4: case 0x00D5: case 0x00D6: return 'O';
    case 0x00F3: case 0x00F2: case 0x00F4: case 0x00F5: case 0x00F6: return 'o';
    case 0x00DA: case 0x00D9: case 0x00DB: case 0x00DC: return 'U';
    case 0x00FA: case 0x00F9: case 0x00FB: case 0x00FC: return 'u';
    case 0x00D1: return 'N';
    case 0x00F1: return 'n';
    default: return '?';
    }
}

/* Helper: check if a UTF-16 code point is an accented Latin character */
static bool u16_is_accented(uint16_t ch) {
    return (ch >= 0x00C0 && ch <= 0x00FF && u16_to_base(ch) != '?');
}

/* Helper: draw a single character or special label at button position within a cell (2x font) */
static void draw_cell_char(uint32_t *fb, uint32_t pitch,
                           int cell_x, int cell_y,
                           int btn_idx, char ch,
                           bool is_selected, bool accent_mode)
{
    bool is_spec = (ch == TG_SPECIAL_SPACE  || ch == TG_SPECIAL_BKSP   ||
                    ch == TG_SPECIAL_ACCENT || ch == TG_SPECIAL_SELALL ||
                    ch == TG_SPECIAL_EXIT   || ch == TG_SPECIAL_CUT    ||
                    ch == TG_SPECIAL_COPY   || ch == TG_SPECIAL_PASTE  ||
                    ch == TG_SPECIAL_CAPS);
    int cw = is_spec ? 32 : 16;  /* 2x: 2-char label = 32px, single char = 16px */

    int ox, oy;
    switch (btn_idx) {
    case TG_BTN_TRIANGLE: ox = CELL_W / 2 - cw / 2; oy = 10;            break;
    case TG_BTN_CIRCLE:   ox = CELL_W - cw - 12;     oy = CELL_H / 2 - 8; break;
    case TG_BTN_CROSS:    ox = CELL_W / 2 - cw / 2; oy = CELL_H - 26;   break;
    case TG_BTN_SQUARE:   ox = 12;                   oy = CELL_H / 2 - 8; break;
    default: return;
    }

    int px = cell_x + ox;
    int py = cell_y + oy;

    uint32_t fg = is_spec ? COL_TEXT_SPECIAL
                : is_selected ? COL_TEXT_HI : COL_TEXT;
    uint32_t bg = COL_BG_DIM;

    if (is_spec) {
        const char *lbl = special_label(ch);
        overlay_draw_text_2x(fb, pitch, px, py, lbl, fg, bg);
    } else {
        overlay_draw_char_2x(fb, pitch, px, py, ch, fg, bg);
        if (accent_mode && is_accentable(ch)) {
            draw_accent_mark_2x(fb, pitch, px, py, COL_TEXT_SPECIAL);
        }
    }
}

/* ─── Main Draw ──────────────────────────────────────────────────── */

void thumbgrid_draw(const ThumbGridState *state, const struct ImeSession *session,
              uint32_t *fb, uint32_t pitch,
              uint32_t screen_w, uint32_t screen_h)
{
    if (!state || !session || !fb) return;
    if (!state->pages) return;
    if (screen_w == 0 || screen_h == 0) return;

    const ImeSession *ses = (const ImeSession *)session;

    /* Center horizontally, vertically in lower third, apply user offset */
    int base_x = ((int)screen_w - OVL_TOTAL_W) / 2 + state->offset_x;
    int base_y = (int)screen_h * 2 / 3 - OVL_TOTAL_H / 2 + state->offset_y;
    if (base_x < 0) base_x = 0;
    if (base_y < 0) base_y = 0;
    if (base_x > (int)screen_w - OVL_TOTAL_W)
        base_x = (int)screen_w - OVL_TOTAL_W;
    if (base_y > (int)screen_h - OVL_TOTAL_H)
        base_y = (int)screen_h - OVL_TOTAL_H;

    const ThumbGridPage *page = &state->pages[state->current_page];

    uint64_t t_start = sceKernelGetProcessTime();  /* PERF */

    /* No full backdrop fill — it was 272K pixels (16.5ms).
     * Cell backgrounds, text bar, and status bar provide their own fills. */

    /* ─── Title bar ─── */
    int cur_y = base_y + 4;
    if (state->title[0] != 0) {
        /* Legacy FB overlay — convert UTF-16 title to ASCII for old renderer */
        char ascii_title[TG_TITLE_MAX];
        for (int i = 0; i < TG_TITLE_MAX; i++) {
            ascii_title[i] = (state->title[i] < 128) ? (char)state->title[i] : '?';
            if (state->title[i] == 0) break;
        }
        overlay_draw_text(fb, pitch, base_x + 8, cur_y + 10, ascii_title,
                          COL_TITLE, COL_BORDER);
    }
    cur_y += TITLE_BAR_H;

    uint64_t t_backdrop = sceKernelGetProcessTime();  /* PERF */

    /* ─── Text display bar ─── */
    int text_y = cur_y;
    int grid_start_x = base_x + (OVL_TOTAL_W - GRID_W) / 2;

    uint32_t text_bg = ses->selected_all ? COL_SELECT_BG : COL_BG_BAR;
    overlay_draw_rect(fb, pitch, base_x + 4, text_y,
                      OVL_TOTAL_W - 8, TEXT_BAR_H, text_bg);

    uint32_t tlen = ses->output_length;
    uint32_t cursor_pos = ses->text_cursor;
    if (cursor_pos > tlen) cursor_pos = tlen;

    /* Display window: 16px per char at 2x, fit in bar width */
    uint32_t display_max = (OVL_TOTAL_W - 48) / 16;
    uint32_t start = 0;
    if (cursor_pos > display_max) {
        start = cursor_pos - display_max;
    }
    uint32_t end = tlen;
    if (end > start + display_max) end = start + display_max;

    int text_char_y = text_y + (TEXT_BAR_H - 16) / 2;

    /* Draw ">" prefix at 2x */
    overlay_draw_char_2x(fb, pitch, base_x + 8, text_char_y, '>',
                         COL_TEXT_SPECIAL, text_bg);

    /* Draw text chars from UTF-16 buffer with accent support */
    int tx = base_x + 32;
    for (uint32_t i = start; i < end; i++) {
        if (i == cursor_pos) {
            /* Thin cursor bar (2px wide) */
            overlay_draw_rect(fb, pitch, tx, text_y + 4, 2, TEXT_BAR_H - 8,
                              COL_CURSOR);
            tx += 4;
        }
        uint16_t ch_val = ses->output[i];
        char base = u16_to_base(ch_val);
        overlay_draw_char_2x(fb, pitch, tx, text_char_y, base,
                             COL_TEXT_BUF, text_bg);
        if (u16_is_accented(ch_val)) {
            draw_accent_mark_2x(fb, pitch, tx, text_char_y, COL_TEXT_SPECIAL);
        }
        tx += 16;
    }
    /* Cursor at end of text */
    if (cursor_pos >= end) {
        overlay_draw_rect(fb, pitch, tx, text_y + 4, 2, TEXT_BAR_H - 8,
                          COL_CURSOR);
    }

    uint64_t t_textbar = sceKernelGetProcessTime();  /* PERF */

    /* ─── Grid ─── */
    int grid_y = text_y + TEXT_BAR_H + 2;

    for (int cell = 0; cell < TG_CELLS; cell++) {
        int row = cell / 3;
        int col = cell % 3;

        int cx = grid_start_x + 1 + col * (CELL_W + 1);
        int cy = grid_y + 1 + row * (CELL_H + 1);

        bool selected = (cell == state->selected_cell);

        /* Cell background fill (also serves as GPU timing padding) */
        overlay_draw_rect(fb, pitch, cx, cy, CELL_W, CELL_H, COL_BG_DIM);

        /* Selected cell: white border highlight (2px) */
        if (selected) {
            draw_cell_border(fb, pitch, cx, cy, CELL_W, CELL_H, COL_BORDER_SEL);
        }

        /* Draw the 4 characters in button positions (2x font) */
        for (int btn = 0; btn < TG_BUTTONS; btn++) {
            char ch = page->chars[cell][btn];
            draw_cell_char(fb, pitch, cx, cy, btn, ch, selected, state->accent_mode);
        }
    }

    uint64_t t_grid = sceKernelGetProcessTime();  /* PERF */

    /* ─── Status bar ─── */
    int page_y = grid_y + GRID_H + 2;
    overlay_draw_rect(fb, pitch, base_x + 4, page_y,
                      OVL_TOTAL_W - 8, PAGE_BAR_H, COL_BG_BAR);

    char page_str[64];
    if (state->accent_mode) {
        snprintf(page_str, sizeof(page_str), "[%s] ACC  L3:a'  L2:shift  R2:done", page->name);
    } else {
        snprintf(page_str, sizeof(page_str), "[%s]  L3:a'  L2:shift  R2:done", page->name);
    }
    overlay_draw_text(fb, pitch, base_x + 8, page_y + 9, page_str,
                      COL_TEXT, COL_BG_BAR);

    /* PERF: throttled breakdown log (once per second) */
    {
        static uint64_t xd_last_log = 0;
        static uint32_t xd_count = 0;
        static uint64_t xd_backdrop_total = 0, xd_textbar_total = 0;
        static uint64_t xd_grid_total = 0, xd_status_total = 0;

        uint64_t t_end = sceKernelGetProcessTime();
        xd_count++;
        xd_backdrop_total += (t_backdrop - t_start);
        xd_textbar_total  += (t_textbar - t_backdrop);
        xd_grid_total     += (t_grid - t_textbar);
        xd_status_total   += (t_end - t_grid);

        if (t_end - xd_last_log >= 1000000) {
            printf("[CIME] DRAW: calls=%u  backdrop=%luus  text=%luus  grid=%luus  status=%luus  total=%luus\n",
                xd_count,
                (unsigned long)(xd_count ? xd_backdrop_total / xd_count : 0),
                (unsigned long)(xd_count ? xd_textbar_total / xd_count : 0),
                (unsigned long)(xd_count ? xd_grid_total / xd_count : 0),
                (unsigned long)(xd_count ? xd_status_total / xd_count : 0),
                (unsigned long)(xd_count ? (xd_backdrop_total + xd_textbar_total + xd_grid_total + xd_status_total) / xd_count : 0));
            xd_last_log = t_end;
            xd_count = 0;
            xd_backdrop_total = xd_textbar_total = xd_grid_total = xd_status_total = 0;
        }
    }
}
