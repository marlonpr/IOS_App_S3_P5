#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "clock_buttons.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_NETWORK_DIAG_TIMEOUT_MS 20000U

typedef enum {
    CLOCK_NETWORK_DIAG_ACTION_NONE = 0,
    CLOCK_NETWORK_DIAG_ACTION_EXIT = 1,
    CLOCK_NETWORK_DIAG_ACTION_SWITCH_MODE = 2,
    CLOCK_NETWORK_DIAG_ACTION_TIMEOUT = 3,
} clock_network_diag_action_t;

typedef struct {
    bool active;
    uint32_t deadline_ms;
} clock_network_diag_state_t;

void clock_network_diag_enter(clock_network_diag_state_t *state,
                              uint32_t now_ms);

bool clock_network_diag_is_active(const clock_network_diag_state_t *state);

clock_network_diag_action_t clock_network_diag_poll(
    clock_network_diag_state_t *state,
    uint32_t now_ms);

clock_network_diag_action_t clock_network_diag_handle_press(
    clock_network_diag_state_t *state,
    button_t button);

#ifdef __cplusplus
}
#endif
