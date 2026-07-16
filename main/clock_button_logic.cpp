#include "clock_button_logic.h"

uint8_t clock_button_state_update(clock_button_state_t *state,
                                  int raw_level,
                                  uint32_t now_ms,
                                  uint32_t normal_hold_ms,
                                  uint32_t long_hold_ms)
{
    if (state == nullptr) {
        return CLOCK_BUTTON_EVENT_NONE;
    }

    const bool is_pressed = raw_level == 0;
    uint8_t events = CLOCK_BUTTON_EVENT_NONE;

    if (is_pressed && !state->pressed) {
        state->pressed = true;
        state->action_fired = false;
        state->pressed_at_ms = now_ms;
        events |= CLOCK_BUTTON_EVENT_PRESS;
    }

    if (!is_pressed && state->pressed) {
        const uint32_t held_ms = now_ms - state->pressed_at_ms;
        events |= CLOCK_BUTTON_EVENT_RELEASE;

        if (!state->action_fired && long_hold_ms != 0 && normal_hold_ms != 0 &&
            held_ms >= normal_hold_ms) {
            events |= CLOCK_BUTTON_EVENT_NORMAL_ACTION;
            state->action_fired = true;
        }

        state->pressed = false;
        state->action_fired = false;
        return events;
    }

    if (!state->pressed || state->action_fired) {
        return events;
    }

    const uint32_t held_ms = now_ms - state->pressed_at_ms;
    if (long_hold_ms != 0) {
        if (held_ms >= long_hold_ms) {
            events |= CLOCK_BUTTON_EVENT_LONG_ACTION;
            state->action_fired = true;
        }
    } else if (normal_hold_ms != 0 && held_ms >= normal_hold_ms) {
        events |= CLOCK_BUTTON_EVENT_NORMAL_ACTION;
        state->action_fired = true;
    }

    return events;
}
