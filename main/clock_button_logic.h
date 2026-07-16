#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CLOCK_BUTTON_EVENT_NONE = 0,
    CLOCK_BUTTON_EVENT_PRESS = 1 << 0,
    CLOCK_BUTTON_EVENT_RELEASE = 1 << 1,
    CLOCK_BUTTON_EVENT_NORMAL_ACTION = 1 << 2,
    CLOCK_BUTTON_EVENT_LONG_ACTION = 1 << 3,
} clock_button_event_t;

typedef struct {
    bool pressed;
    bool action_fired;
    uint32_t pressed_at_ms;
} clock_button_state_t;

/*
 * Decode one independently sampled, active-low button.
 * long_hold_ms == 0 selects the normal action while held. A non-zero
 * long_hold_ms selects a long action while held and, when normal_hold_ms is
 * non-zero, defers the normal action until release. normal_hold_ms == 0
 * disables the normal action entirely.
 */
uint8_t clock_button_state_update(clock_button_state_t *state,
                                  int raw_level,
                                  uint32_t now_ms,
                                  uint32_t normal_hold_ms,
                                  uint32_t long_hold_ms);
