#include "clock_network_indicator.h"

bool clock_network_indicator_is_visible(clock_network_status_t status,
                                        int64_t now_us)
{
    if (status == CLOCK_NETWORK_ONLINE || status == CLOCK_NETWORK_OFFLINE) {
        return true;
    }

    if (status != CLOCK_NETWORK_CONNECTING || now_us < 0) {
        return false;
    }

    return ((now_us / CLOCK_NETWORK_BLINK_HALF_PERIOD_US) & 1LL) == 0;
}

size_t clock_network_indicator_build_pixels(
    uint16_t width,
    uint16_t height,
    clock_network_status_t status,
    int64_t now_us,
    clock_network_indicator_pixel_t *pixels,
    size_t capacity)
{
    if (width < 2 || height < 2 || pixels == nullptr ||
        capacity < CLOCK_NETWORK_INDICATOR_PIXEL_COUNT ||
        !clock_network_indicator_is_visible(status, now_us)) {
        return 0;
    }

    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    switch (status) {
        case CLOCK_NETWORK_ONLINE:
            green = 255;
            break;
        case CLOCK_NETWORK_OFFLINE:
            red = 255;
            break;
        case CLOCK_NETWORK_CONNECTING:
            red = 255;
            green = 255;
            blue = 255;
            break;
        default:
            return 0;
    }

    const uint16_t right = static_cast<uint16_t>(width - 1);
    const uint16_t right_inner = static_cast<uint16_t>(width - 2);

    const uint16_t coordinates[CLOCK_NETWORK_INDICATOR_PIXEL_COUNT][2] = {
        {right, 0}, {right_inner, 0}, {right, 1},
    };

    for (size_t index = 0; index < CLOCK_NETWORK_INDICATOR_PIXEL_COUNT;
         ++index) {
        pixels[index] = {
            .x = coordinates[index][0],
            .y = coordinates[index][1],
            .red = red,
            .green = green,
            .blue = blue,
        };
    }

    return CLOCK_NETWORK_INDICATOR_PIXEL_COUNT;
}
