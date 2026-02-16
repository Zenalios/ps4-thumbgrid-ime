/**
 * @file xti.h
 * @brief XTI 3x3 grid text input engine
 *
 * Analog stick selects one of 9 cells, face buttons input characters.
 * Center cell (4): Triangle=Space, Circle=Exit IME, Cross=Select All, Square=Backspace.
 * R2=submit, L2=shift hold, L1/R1=letters/symbols toggle, D-pad=text cursor.
 * L2+center: Triangle=Paste, Circle=Caps Lock, Cross=Cut, Square=Copy.
 * L3=accent toggle.
 */

#ifndef XTI_H
#define XTI_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration */
struct ImeSession;

/* ─── Constants ──────────────────────────────────────────────────── */

#define XTI_CELLS       9
#define XTI_BUTTONS     4   /* triangle, circle, cross, square */
#define XTI_MAX_PAGES   3
#define XTI_CENTER_CELL 4

/* Button indices for character lookup */
#define XTI_BTN_TRIANGLE  0
#define XTI_BTN_CIRCLE    1
#define XTI_BTN_CROSS     2
#define XTI_BTN_SQUARE    3

/* Special function markers (stored as chars, never displayed) */
#define XTI_SPECIAL_BKSP    '\x02'
#define XTI_SPECIAL_SPACE   '\x03'
#define XTI_SPECIAL_ACCENT  '\x04'
#define XTI_SPECIAL_SELALL  '\x05'
#define XTI_SPECIAL_EXIT    '\x06'
#define XTI_SPECIAL_CUT     '\x07'
#define XTI_SPECIAL_COPY    '\x08'
#define XTI_SPECIAL_PASTE   '\x09'
#define XTI_SPECIAL_CAPS    '\x0A'

/* ─── Structures ─────────────────────────────────────────────────── */

typedef struct XtiPage {
    const char *name;                           /* "abc", "ABC", "123" */
    char        chars[XTI_CELLS][XTI_BUTTONS];  /* [cell][button] = character */
} XtiPage;

#define XTI_TITLE_MAX  48

typedef struct XtiState {
    int32_t        selected_cell;   /* 0-8, from analog stick */
    int32_t        current_page;    /* 0-2 */
    int32_t        page_count;
    const XtiPage *pages;           /* pointer to static page array */
    int32_t        offset_x;        /* widget position offset from default center */
    int32_t        offset_y;
    bool           accent_mode;    /* true = vowels produce accented variants */
    uint16_t       title[XTI_TITLE_MAX];  /* label shown above text bar (UTF-16) */
} XtiState;

/* ─── Functions ──────────────────────────────────────────────────── */

void    xti_init(XtiState *state);
void    xti_select_cell(XtiState *state, uint8_t stick_x, uint8_t stick_y);
char    xti_get_char(const XtiState *state, int button_index);
bool    xti_is_special(const XtiState *state, int button_index);
void    xti_shift_toggle(XtiState *state);
void    xti_toggle_symbols(XtiState *state);
void    xti_toggle_accent(XtiState *state);
uint16_t xti_accent_lookup(char base);
void    xti_update_position(XtiState *state, uint8_t rstick_x, uint8_t rstick_y,
                            uint32_t screen_w, uint32_t screen_h);

void    xti_draw(const XtiState *state, const struct ImeSession *session,
                 uint32_t *fb, uint32_t pitch,
                 uint32_t screen_w, uint32_t screen_h);

#endif /* XTI_H */
