#pragma once

#include <stdint.h>

typedef enum {
    CLOCK_MENU_FIELD_IDLE = 0,
    CLOCK_MENU_FIELD_BRIGHTNESS,
    CLOCK_MENU_FIELD_FORMAT,
    CLOCK_MENU_FIELD_HOUR,
    CLOCK_MENU_FIELD_MINUTE,
    CLOCK_MENU_FIELD_DAY,
    CLOCK_MENU_FIELD_MONTH,
    CLOCK_MENU_FIELD_YEAR,
} clock_menu_field_t;

clock_menu_field_t clock_menu_next_field(clock_menu_field_t field);
uint8_t clock_menu_toggle_format(uint8_t format);
