#pragma once

#include <stdint.h>

#include "esp_err.h"

void clock_modes_reset_sequences(void);
uint8_t clock_modes_advance_mode(void);
esp_err_t clock_modes_set_mode(uint8_t mode);
uint8_t clock_modes_get_mode(void);
