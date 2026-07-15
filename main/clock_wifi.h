#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t clock_wifi_init_sta(void);
bool clock_wifi_wait_for_ip(uint32_t timeout_ms);
bool clock_wifi_route_is_ready(void);
esp_err_t clock_wifi_stop(void);

#ifdef __cplusplus
}
#endif
