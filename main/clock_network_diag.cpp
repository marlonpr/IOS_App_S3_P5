#include "clock_network_diag.h"

void clock_network_diag_enter(clock_network_diag_state_t *state,
                              uint32_t now_ms)
{
    if (state == nullptr) {
        return;
    }

    state->active = true;
    state->deadline_ms = now_ms + CLOCK_NETWORK_DIAG_TIMEOUT_MS;
}

bool clock_network_diag_is_active(const clock_network_diag_state_t *state)
{
    return state != nullptr && state->active;
}

clock_network_diag_action_t clock_network_diag_poll(
    clock_network_diag_state_t *state,
    uint32_t now_ms)
{
    if (state == nullptr || !state->active) {
        return CLOCK_NETWORK_DIAG_ACTION_NONE;
    }

    if (static_cast<int32_t>(now_ms - state->deadline_ms) < 0) {
        return CLOCK_NETWORK_DIAG_ACTION_NONE;
    }

    state->active = false;
    return CLOCK_NETWORK_DIAG_ACTION_TIMEOUT;
}

clock_network_diag_action_t clock_network_diag_handle_press(
    clock_network_diag_state_t *state,
    button_t button)
{
    if (state == nullptr || !state->active) {
        return CLOCK_NETWORK_DIAG_ACTION_NONE;
    }

    state->active = false;
    return button == BTN_DOWN
               ? CLOCK_NETWORK_DIAG_ACTION_SWITCH_MODE
               : CLOCK_NETWORK_DIAG_ACTION_EXIT;
}
