#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_LOGO_WIDTH 64
#define CLOCK_LOGO_HEIGHT 32
#define CLOCK_LOGO_PIXEL_COUNT 2048
#define CLOCK_LOGO_PAYLOAD_BYTES 6144

void clock_logo_init();

const uint32_t *clock_logo_lock_pixels();
void clock_logo_unlock_pixels();

esp_err_t clock_logo_reload_from_sd();

uint32_t clock_logo_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t clock_logo_crc32_finalize(uint32_t crc);

#ifdef __cplusplus
}
#endif
