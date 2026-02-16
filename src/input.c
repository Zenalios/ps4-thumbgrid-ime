/**
 * @file input.c
 * @brief PS4 controller input processing for ThumbGrid input
 */

#include "plugin_common.h"
#include "input.h"

void input_update(InputState *state, uint32_t raw_buttons,
                  uint8_t stick_x, uint8_t stick_y,
                  uint8_t rstick_x, uint8_t rstick_y,
                  uint64_t timestamp_us) {
    if (!state) {
        return;
    }
    state->buttons_previous = state->buttons_current;
    state->buttons_current  = raw_buttons;
    state->timestamp_us     = timestamp_us;
    state->buttons_pressed  = raw_buttons & ~state->buttons_previous;
    state->buttons_released = state->buttons_previous & ~raw_buttons;
    state->stick_x          = stick_x;
    state->stick_y          = stick_y;
    state->rstick_x         = rstick_x;
    state->rstick_y         = rstick_y;
}

ImeAction input_get_action(const InputState *state) {
    if (!state) {
        return IME_ACTION_NONE;
    }
    /* Priority: Cancel > Submit > Face buttons > Cursor > Page
     * NOTE: L2 (shift/caps) handled via analog trigger in ime_hook.c
     * NOTE: X (cross) handled via hold-state machine in ime_hook.c */
    if (state->buttons_pressed & PAD_BUTTON_OPTIONS) {
        return IME_ACTION_CANCEL;
    }
    if (state->buttons_pressed & PAD_BUTTON_R2) {
        return IME_ACTION_SUBMIT;
    }
    if (state->buttons_pressed & PAD_BUTTON_TRIANGLE) {
        return IME_ACTION_FACE_TRIANGLE;
    }
    if (state->buttons_pressed & PAD_BUTTON_CIRCLE) {
        return IME_ACTION_FACE_CIRCLE;
    }
    if (state->buttons_pressed & PAD_BUTTON_SQUARE) {
        return IME_ACTION_FACE_SQUARE;
    }
    if (state->buttons_pressed & PAD_BUTTON_UP) {
        return IME_ACTION_CURSOR_HOME;
    }
    if (state->buttons_pressed & PAD_BUTTON_DOWN) {
        return IME_ACTION_CURSOR_END;
    }
    if (state->buttons_pressed & PAD_BUTTON_LEFT) {
        return IME_ACTION_CURSOR_LEFT;
    }
    if (state->buttons_pressed & PAD_BUTTON_RIGHT) {
        return IME_ACTION_CURSOR_RIGHT;
    }
    if (state->buttons_pressed & PAD_BUTTON_R1) {
        return IME_ACTION_PAGE_NEXT;
    }
    if (state->buttons_pressed & PAD_BUTTON_L1) {
        return IME_ACTION_PAGE_PREV;
    }
    return IME_ACTION_NONE;
}

bool input_just_pressed(const InputState *state, uint32_t button) {
    if (!state) {
        return false;
    }
    return (state->buttons_pressed & button) != 0;
}

bool input_is_held(const InputState *state, uint32_t button) {
    if (!state) {
        return false;
    }
    return (state->buttons_current & button) != 0;
}
