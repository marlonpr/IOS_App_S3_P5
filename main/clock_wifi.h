#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "clock_network_events.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t clock_wifi_init_sta(void);
bool clock_wifi_wait_for_ip(uint32_t timeout_ms);
bool clock_wifi_route_is_ready(void);
esp_err_t clock_wifi_stop(void);
void clock_wifi_set_event_callback(
    clock_network_interface_event_callback_t callback,
    void *context);

#ifdef __cplusplus
}
#endif
