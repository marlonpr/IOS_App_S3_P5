#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t clock_sd_card_init();
bool clock_sd_card_is_mounted();

#ifdef __cplusplus
}
#endif
