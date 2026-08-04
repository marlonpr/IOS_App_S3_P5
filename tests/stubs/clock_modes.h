#pragma once

#include <stdint.h>

#include "esp_err.h"

#define CLOCK_DEFAULT_LOGO_INTERVAL_SECONDS ((uint16_t)3)
#define CLOCK_DEFAULT_MODE_INTERVAL_SECONDS ((uint16_t)21)
#define CLOCK_MODE_INTERVAL_MIN_SECONDS ((uint16_t)1)
#define CLOCK_MODE_INTERVAL_MAX_SECONDS ((uint16_t)9999)

typedef struct {
    uint16_t logo_seconds;
    uint16_t mode_seconds;
} clock_mode_intervals_t;

void clock_modes_reset_sequences(void);
uint8_t clock_modes_advance_mode(void);
esp_err_t clock_modes_set_mode(uint8_t mode);
uint8_t clock_modes_get_mode(void);
clock_mode_intervals_t clock_modes_get_intervals(void);
bool clock_modes_validate_intervals(const clock_mode_intervals_t *intervals);
esp_err_t clock_modes_set_intervals(const clock_mode_intervals_t *intervals);
esp_err_t clock_modes_restore_default_intervals(void);
