#include "clock_network.h"

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
