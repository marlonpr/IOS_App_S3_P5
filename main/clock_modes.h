#pragma once

#include <stdint.h>

#include "esp_err.h"

#define CLOCK_DEFAULT_LOGO_INTERVAL_SECONDS ((uint16_t)3)
#define CLOCK_DEFAULT_MODE_INTERVAL_SECONDS ((uint16_t)21)
#define CLOCK_MIN_DISPLAY_INTERVAL_SECONDS ((uint16_t)1)
#define CLOCK_MAX_DISPLAY_INTERVAL_SECONDS ((uint16_t)3600)

typedef struct {
    uint16_t logo_seconds;
    uint16_t mode_seconds;
} clock_mode_intervals_t;

typedef enum {
    MODE_1 = 1,
    MODE_2,
    MODE_3,
    MODE_ROTATION,
} display_mode_t;

typedef enum {
    ROT_ITEM_LOGO = 0,
    ROT_ITEM_MODE_1,
    ROT_ITEM_MODE_2,
    ROT_ITEM_MODE_3,
} rotation_item_t;

typedef enum {
    FIXED_ITEM_LOGO = 0,
    FIXED_ITEM_SCREEN,
} fixed_item_t;

rotation_item_t clock_modes_get_rotation_item(void);
fixed_item_t clock_modes_get_fixed_item(void);
void clock_modes_reset_sequences(void);

esp_err_t clock_modes_init_intervals(void);
clock_mode_intervals_t clock_modes_get_intervals(void);
bool clock_modes_validate_intervals(const clock_mode_intervals_t *intervals);
esp_err_t clock_modes_set_intervals(const clock_mode_intervals_t *intervals);
esp_err_t clock_modes_restore_default_intervals(void);
