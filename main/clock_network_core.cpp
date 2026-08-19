#include "clock_network.h"

#include <stdio.h>
#include <string.h>

bool clock_network_mode_is_valid(uint8_t mode)
{
    return mode <= CLOCK_NETWORK_MODE_WIFI;
}

clock_network_mode_t clock_network_next_mode(clock_network_mode_t mode)
{
    switch (mode) {
        case CLOCK_NETWORK_MODE_AUTO:
            return CLOCK_NETWORK_MODE_ETHERNET;
        case CLOCK_NETWORK_MODE_ETHERNET:
            return CLOCK_NETWORK_MODE_WIFI;
        case CLOCK_NETWORK_MODE_WIFI:
        default:
            return CLOCK_NETWORK_MODE_AUTO;
    }
}

const char *clock_network_mode_label(clock_network_mode_t mode)
{
    switch (mode) {
        case CLOCK_NETWORK_MODE_ETHERNET:
            return "Ethernet";
        case CLOCK_NETWORK_MODE_WIFI:
            return "Wi-Fi";
        case CLOCK_NETWORK_MODE_AUTO:
        default:
            return "Auto";
    }
}

const char *clock_network_mode_panel_label(clock_network_mode_t mode)
{
    switch (mode) {
        case CLOCK_NETWORK_MODE_ETHERNET:
            return "NET: ETH";
        case CLOCK_NETWORK_MODE_WIFI:
            return "NET: WIFI";
        case CLOCK_NETWORK_MODE_AUTO:
        default:
            return "NET: AUTO";
    }
}

const char *clock_network_result_message(clock_network_interface_t interface)
{
    switch (interface) {
        case CLOCK_NETWORK_INTERFACE_ETHERNET:
            return "ETH CONNECTED";
        case CLOCK_NETWORK_INTERFACE_WIFI:
            return "WIFI CONNECTED";
        case CLOCK_NETWORK_INTERFACE_NONE:
        default:
            return "NET NOT CONNECTED";
    }
}

bool clock_network_is_result_message(const char *message)
{
    if (message == nullptr) {
        return false;
    }

    return strcmp(message, "ETH CONNECTED") == 0 ||
           strcmp(message, "WIFI CONNECTED") == 0 ||
           strcmp(message, "NET NOT CONNECTED") == 0;
}

void clock_network_result_gate_init(clock_network_result_gate_t *gate)
{
    if (gate == nullptr) {
        return;
    }

    gate->startup_complete = false;
    gate->result_pending = false;
    gate->pending_message[0] = '\0';
    gate->pending_duration_ms = 0;
}

bool clock_network_result_gate_submit(clock_network_result_gate_t *gate,
                                      const char *message,
                                      uint32_t duration_ms)
{
    if (gate == nullptr || !clock_network_is_result_message(message)) {
        return true;
    }

    if (gate->startup_complete) {
        return true;
    }

    snprintf(gate->pending_message,
             sizeof(gate->pending_message),
             "%s",
             message);
    gate->pending_duration_ms = duration_ms;
    gate->result_pending = true;
    return false;
}

bool clock_network_result_gate_finish_startup(
    clock_network_result_gate_t *gate,
    char *message,
    size_t message_capacity,
    uint32_t *duration_ms)
{
    if (gate == nullptr) {
        return false;
    }

    gate->startup_complete = true;

    if (!gate->result_pending || message == nullptr || message_capacity == 0 ||
        duration_ms == nullptr) {
        return false;
    }

    snprintf(message, message_capacity, "%s", gate->pending_message);
    *duration_ms = gate->pending_duration_ms;
    gate->result_pending = false;
    return true;
}

clock_network_interface_t clock_network_first_interface(clock_network_mode_t mode)
{
    if (mode == CLOCK_NETWORK_MODE_WIFI) {
        return CLOCK_NETWORK_INTERFACE_WIFI;
    }

    return CLOCK_NETWORK_INTERFACE_ETHERNET;
}

clock_network_interface_t clock_network_fallback_interface(
    clock_network_mode_t mode,
    clock_network_interface_t failed_interface)
{
    if (mode == CLOCK_NETWORK_MODE_AUTO &&
        failed_interface == CLOCK_NETWORK_INTERFACE_ETHERNET) {
        return CLOCK_NETWORK_INTERFACE_WIFI;
    }

    return CLOCK_NETWORK_INTERFACE_NONE;
}

bool clock_network_service_guard_try_start(clock_network_service_guard_t *guard)
{
    if (guard == nullptr || guard->started) {
        return false;
    }

    guard->started = true;
    return true;
}

bool clock_network_long_hold_update(clock_network_long_hold_t *state,
                                    bool pressed,
                                    uint32_t held_ms)
{
    if (state == nullptr) {
        return false;
    }

    if (!pressed) {
        state->fired = false;
        return false;
    }

    if (!state->fired && held_ms >= 10000) {
        state->fired = true;
        return true;
    }

    return false;
}

void clock_network_retry_state_init(clock_network_retry_state_t *state,
                                    uint64_t now_ms)
{
    clock_network_retry_start_burst(state, now_ms);
}

void clock_network_retry_start_burst(clock_network_retry_state_t *state,
                                     uint64_t now_ms)
{
    if (state == nullptr) {
        return;
    }

    state->status = CLOCK_NETWORK_CONNECTING;
    state->attempt = 0;
    state->attempt_complete = true;
    state->burst_started_ms = now_ms;
    state->next_action_ms = now_ms;
}

bool clock_network_retry_attempt_is_due(
    const clock_network_retry_state_t *state,
    uint64_t now_ms)
{
    return state != nullptr &&
           state->status == CLOCK_NETWORK_CONNECTING &&
           state->attempt_complete &&
           state->attempt < CLOCK_NETWORK_ATTEMPTS_PER_BURST &&
           now_ms >= state->next_action_ms;
}

uint8_t clock_network_retry_begin_attempt(clock_network_retry_state_t *state)
{
    if (state == nullptr ||
        state->status != CLOCK_NETWORK_CONNECTING ||
        !state->attempt_complete ||
        state->attempt >= CLOCK_NETWORK_ATTEMPTS_PER_BURST) {
        return 0;
    }

    ++state->attempt;
    state->attempt_complete = false;
    if (state->attempt < CLOCK_NETWORK_ATTEMPTS_PER_BURST) {
        state->next_action_ms =
            state->burst_started_ms +
            static_cast<uint64_t>(state->attempt) *
                CLOCK_NETWORK_RETRY_SLOT_MS;
    } else {
        /*
         * The final attempt starts at the last retry boundary.  Keep that
         * boundary here so its actual completion, rather than a synthetic
         * fourth slot ending at 80 seconds, controls burst exhaustion.
         */
        state->next_action_ms =
            state->burst_started_ms +
            CLOCK_NETWORK_FINAL_ATTEMPT_START_MS;
    }
    return state->attempt;
}

void clock_network_retry_attempt_failed(clock_network_retry_state_t *state)
{
    if (state == nullptr ||
        state->status != CLOCK_NETWORK_CONNECTING ||
        state->attempt == 0 ||
        state->attempt_complete) {
        return;
    }

    state->attempt_complete = true;
}

bool clock_network_retry_burst_is_exhausted(
    const clock_network_retry_state_t *state,
    uint64_t now_ms)
{
    return state != nullptr &&
           state->status == CLOCK_NETWORK_CONNECTING &&
           state->attempt_complete &&
           state->attempt == CLOCK_NETWORK_ATTEMPTS_PER_BURST &&
           now_ms >= state->next_action_ms;
}

void clock_network_retry_enter_cooldown(clock_network_retry_state_t *state,
                                        uint64_t now_ms)
{
    if (state == nullptr) {
        return;
    }

    state->status = CLOCK_NETWORK_OFFLINE;
    state->attempt = 0;
    state->attempt_complete = true;
    state->burst_started_ms = 0;
    state->next_action_ms = now_ms + CLOCK_NETWORK_COOLDOWN_MS;
}

bool clock_network_retry_cooldown_is_expired(
    const clock_network_retry_state_t *state,
    uint64_t now_ms)
{
    return state != nullptr &&
           state->status == CLOCK_NETWORK_OFFLINE &&
           now_ms >= state->next_action_ms;
}

void clock_network_retry_connected(clock_network_retry_state_t *state)
{
    if (state == nullptr) {
        return;
    }

    state->status = CLOCK_NETWORK_ONLINE;
    state->attempt = 0;
    state->attempt_complete = true;
    state->burst_started_ms = 0;
    state->next_action_ms = 0;
}

void clock_network_retry_disconnected(clock_network_retry_state_t *state,
                                      uint64_t now_ms)
{
    if (state == nullptr) {
        return;
    }

    clock_network_retry_start_burst(state, now_ms);
}

esp_err_t clock_network_cycle_saved_mode(
    clock_network_mode_t current_mode,
    clock_network_save_mode_callback_t save_callback,
    void *save_context,
    clock_network_mode_t *new_mode)
{
    if (!clock_network_mode_is_valid(static_cast<uint8_t>(current_mode)) ||
        save_callback == nullptr || new_mode == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const clock_network_mode_t next_mode =
        clock_network_next_mode(current_mode);
    const esp_err_t result =
        save_callback(static_cast<uint8_t>(next_mode), save_context);

    if (result == ESP_OK) {
        *new_mode = next_mode;
    }

    return result;
}
