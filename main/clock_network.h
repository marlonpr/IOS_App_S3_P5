#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "clock_ethernet.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOCK_NETWORK_MODE_AUTO = 0,
    CLOCK_NETWORK_MODE_ETHERNET = 1,
    CLOCK_NETWORK_MODE_WIFI = 2,
} clock_network_mode_t;

typedef enum {
    CLOCK_NETWORK_INTERFACE_NONE = 0,
    CLOCK_NETWORK_INTERFACE_ETHERNET = 1,
    CLOCK_NETWORK_INTERFACE_WIFI = 2,
} clock_network_interface_t;

typedef struct {
    bool started;
} clock_network_service_guard_t;

typedef struct {
    bool fired;
} clock_network_long_hold_t;

typedef esp_err_t (*clock_network_save_mode_callback_t)(uint8_t mode,
                                                         void *context);
typedef void (*clock_network_show_message_callback_t)(const char *message,
                                                       uint32_t duration_ms);

bool clock_network_mode_is_valid(uint8_t mode);
clock_network_mode_t clock_network_next_mode(clock_network_mode_t mode);
const char *clock_network_mode_label(clock_network_mode_t mode);
const char *clock_network_mode_panel_label(clock_network_mode_t mode);
const char *clock_network_result_message(clock_network_interface_t interface);

clock_network_interface_t clock_network_first_interface(clock_network_mode_t mode);
clock_network_interface_t clock_network_fallback_interface(
    clock_network_mode_t mode,
    clock_network_interface_t failed_interface);

bool clock_network_service_guard_try_start(clock_network_service_guard_t *guard);
bool clock_network_long_hold_update(clock_network_long_hold_t *state,
                                    bool pressed,
                                    uint32_t held_ms);

esp_err_t clock_network_cycle_saved_mode(
    clock_network_mode_t current_mode,
    clock_network_save_mode_callback_t save_callback,
    void *save_context,
    clock_network_mode_t *new_mode);

esp_err_t clock_network_start(clock_network_mode_t boot_mode,
                              clock_ethernet_rx_callback_t rx_callback,
                              clock_network_show_message_callback_t show_message);

clock_network_mode_t clock_network_get_boot_mode(void);
clock_network_interface_t clock_network_get_active_interface(void);
bool clock_network_services_started(void);

#ifdef __cplusplus
}
#endif
