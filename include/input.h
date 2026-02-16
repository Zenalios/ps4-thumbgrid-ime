/**
 * @file input.h
 * @brief PS4 controller input mapping for XTI grid input
 */

#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

/* PS4 button masks */
#define PAD_BUTTON_L3         0x00000002
#define PAD_BUTTON_OPTIONS    0x00000008
#define PAD_BUTTON_UP         0x00000010
#define PAD_BUTTON_RIGHT      0x00000020
#define PAD_BUTTON_DOWN       0x00000040
#define PAD_BUTTON_LEFT       0x00000080
#define PAD_BUTTON_L2         0x00000100
#define PAD_BUTTON_R2         0x00000200
#define PAD_BUTTON_L1         0x00000400
#define PAD_BUTTON_R1         0x00000800
#define PAD_BUTTON_TRIANGLE   0x00001000
#define PAD_BUTTON_CIRCLE     0x00002000
#define PAD_BUTTON_CROSS      0x00004000
#define PAD_BUTTON_SQUARE     0x00008000

typedef enum ImeAction {
    IME_ACTION_NONE = 0,
    IME_ACTION_FACE_TRIANGLE,
    IME_ACTION_FACE_CIRCLE,
    IME_ACTION_FACE_CROSS,
    IME_ACTION_FACE_SQUARE,
    IME_ACTION_SUBMIT,
    IME_ACTION_SHIFT,
    IME_ACTION_CURSOR_LEFT,
    IME_ACTION_CURSOR_RIGHT,
    IME_ACTION_CURSOR_HOME,
    IME_ACTION_CURSOR_END,
    IME_ACTION_PAGE_NEXT,
    IME_ACTION_PAGE_PREV,
    IME_ACTION_CANCEL,
} ImeAction;

typedef struct InputState {
    uint32_t buttons_current;
    uint32_t buttons_previous;
    uint32_t buttons_pressed;
    uint32_t buttons_released;
    uint64_t timestamp_us;
    uint8_t  stick_x;     /* raw left stick X: 0-255, 128=center */
    uint8_t  stick_y;     /* raw left stick Y: 0-255, 128=center */
    uint8_t  rstick_x;    /* raw right stick X: 0-255, 128=center */
    uint8_t  rstick_y;    /* raw right stick Y: 0-255, 128=center */
} InputState;

void      input_update(InputState *state, uint32_t raw_buttons,
                       uint8_t stick_x, uint8_t stick_y,
                       uint8_t rstick_x, uint8_t rstick_y,
                       uint64_t timestamp_us);
ImeAction input_get_action(const InputState *state);
bool      input_just_pressed(const InputState *state, uint32_t button);
bool      input_is_held(const InputState *state, uint32_t button);

#endif /* INPUT_H */
