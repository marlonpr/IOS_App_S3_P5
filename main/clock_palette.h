#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_PALETTE_MAX_ROLES 7
#define CLOCK_PALETTE_SCHEMA_VERSION 1

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} clock_rgb_t;

typedef enum {
    CLOCK_PALETTE_MODE_1 = 1,
    CLOCK_PALETTE_MODE_2 = 2,
    CLOCK_PALETTE_MODE_3 = 3,
} clock_palette_mode_id_t;

typedef enum {
    CLOCK_PALETTE_ROLE_TIME = 0x01,
    CLOCK_PALETTE_ROLE_DATE = 0x02,
    CLOCK_PALETTE_ROLE_WEEKDAY = 0x03,
    CLOCK_PALETTE_ROLE_TEMPERATURE_COLD = 0x10,
    CLOCK_PALETTE_ROLE_TEMPERATURE_COOL = 0x11,
    CLOCK_PALETTE_ROLE_TEMPERATURE_WARM = 0x12,
    CLOCK_PALETTE_ROLE_TEMPERATURE_HOT = 0x13,
} clock_palette_role_id_t;

typedef struct {
    uint8_t role;
    clock_rgb_t color;
} clock_palette_entry_t;

typedef struct {
    uint8_t mode;
    uint8_t count;
    clock_palette_entry_t entries[CLOCK_PALETTE_MAX_ROLES];
} clock_mode_palette_t;

/* Initializes compiled defaults in RAM, then overlays valid NVS entries. */
esp_err_t clock_palette_init(void);
esp_err_t clock_palette_load_from_nvs(void);

bool clock_palette_is_supported_mode(uint8_t mode);
bool clock_palette_is_supported_role(uint8_t mode, uint8_t role);

bool clock_palette_get_color(uint8_t mode,
                             uint8_t role,
                             clock_rgb_t *out_color);
bool clock_palette_get_mode_snapshot(uint8_t mode,
                                     clock_mode_palette_t *out_palette);
bool clock_palette_get_factory_snapshot(uint8_t mode,
                                        clock_mode_palette_t *out_palette);
bool clock_palette_snapshot_get_color(const clock_mode_palette_t *palette,
                                      uint8_t role,
                                      clock_rgb_t *out_color);
bool clock_palette_snapshot_get_temperature_color(
    const clock_mode_palette_t *palette,
    float temp_c,
    clock_rgb_t *out_color);

/*
 * Save accepts one complete effective palette for the selected mode. The NVS
 * blob contains only entries that differ from compiled defaults.
 */
esp_err_t clock_palette_save_mode_override(
    uint8_t mode,
    const clock_palette_entry_t *entries,
    size_t count);

esp_err_t clock_palette_restore_mode_defaults(uint8_t mode);
esp_err_t clock_palette_restore_all_defaults(void);

#ifdef __cplusplus
}
#endif
