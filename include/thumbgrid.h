/**
 * @file thumbgrid.h
 * @brief ThumbGrid 3x3 grid text input engine
 *
 * Analog stick selects one of 9 cells, face buttons input characters.
 * Center cell (4): Triangle=Space, Circle=Exit IME, Cross=Select All, Square=Backspace.
 * R2=submit, L2=shift hold, L1/R1=letters/symbols toggle, D-pad=text cursor.
 * L2+center: Triangle=Paste, Circle=Caps Lock, Cross=Cut, Square=Copy.
 * L3=accent toggle.
 */

#ifndef THUMBGRID_H
#define THUMBGRID_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration */
struct ImeSession;

/* ─── Constants ──────────────────────────────────────────────────── */

#define TG_CELLS       9
#define TG_BUTTONS     4   /* triangle, circle, cross, square */
#define TG_MAX_PAGES   3
#define TG_CENTER_CELL 4

/* Button indices for character lookup */
#define TG_BTN_TRIANGLE  0
#define TG_BTN_CIRCLE    1
#define TG_BTN_CROSS     2
#define TG_BTN_SQUARE    3

/* Special function markers (stored as chars, never displayed) */
#define TG_SPECIAL_BKSP    '\x02'
#define TG_SPECIAL_SPACE   '\x03'
#define TG_SPECIAL_ACCENT  '\x04'
#define TG_SPECIAL_SELALL  '\x05'
#define TG_SPECIAL_EXIT    '\x06'
#define TG_SPECIAL_CUT     '\x07'
#define TG_SPECIAL_COPY    '\x08'
#define TG_SPECIAL_PASTE   '\x09'
#define TG_SPECIAL_CAPS    '\x0A'

/* ─── Structures ─────────────────────────────────────────────────── */

typedef struct ThumbGridPage {
    const char *name;                           /* "abc", "ABC", "123" */
    char        chars[TG_CELLS][TG_BUTTONS];  /* [cell][button] = character */
} ThumbGridPage;

#define TG_TITLE_MAX  48

typedef struct ThumbGridState {
    int32_t        selected_cell;   /* 0-8, from analog stick */
    int32_t        current_page;    /* 0-2 */
    int32_t        page_count;
    const ThumbGridPage *pages;           /* pointer to static page array */
    int32_t        offset_x;        /* widget position offset from default center */
    int32_t        offset_y;
    bool           accent_mode;    /* true = vowels produce accented variants */
    uint16_t       title[TG_TITLE_MAX];  /* label shown above text bar (UTF-16) */
} ThumbGridState;

/* ─── Functions ──────────────────────────────────────────────────── */

void    thumbgrid_init(ThumbGridState *state);
void    thumbgrid_select_cell(ThumbGridState *state, uint8_t stick_x, uint8_t stick_y);
char    thumbgrid_get_char(const ThumbGridState *state, int button_index);
bool    thumbgrid_is_special(const ThumbGridState *state, int button_index);
void    thumbgrid_shift_toggle(ThumbGridState *state);
void    thumbgrid_toggle_symbols(ThumbGridState *state);
void    thumbgrid_toggle_accent(ThumbGridState *state);
uint16_t thumbgrid_accent_lookup(char base);
void    thumbgrid_update_position(ThumbGridState *state, uint8_t rstick_x, uint8_t rstick_y,
                            uint32_t screen_w, uint32_t screen_h);

void    thumbgrid_draw(const ThumbGridState *state, const struct ImeSession *session,
                 uint32_t *fb, uint32_t pitch,
                 uint32_t screen_w, uint32_t screen_h);

#endif /* THUMBGRID_H */
