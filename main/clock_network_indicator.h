#pragma once

#include <stddef.h>
#include <stdint.h>

#include "clock_network.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_NETWORK_INDICATOR_PIXEL_COUNT 3U
#define CLOCK_NETWORK_BLINK_HALF_PERIOD_US 500000LL

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} clock_network_indicator_pixel_t;

bool clock_network_indicator_is_visible(clock_network_status_t status,
                                        int64_t now_us);

size_t clock_network_indicator_build_pixels(
    uint16_t width,
    uint16_t height,
    clock_network_status_t status,
    int64_t now_us,
    clock_network_indicator_pixel_t *pixels,
    size_t capacity);

#ifdef __cplusplus
}
#endif
