#include "clock_menu_model.h"

clock_menu_field_t clock_menu_next_field(clock_menu_field_t field)
{
    switch (field) {
        case CLOCK_MENU_FIELD_BRIGHTNESS:
            return CLOCK_MENU_FIELD_FORMAT;
        case CLOCK_MENU_FIELD_FORMAT:
            return CLOCK_MENU_FIELD_HOUR;
        case CLOCK_MENU_FIELD_HOUR:
            return CLOCK_MENU_FIELD_MINUTE;
        case CLOCK_MENU_FIELD_MINUTE:
            return CLOCK_MENU_FIELD_DAY;
        case CLOCK_MENU_FIELD_DAY:
            return CLOCK_MENU_FIELD_MONTH;
        case CLOCK_MENU_FIELD_MONTH:
            return CLOCK_MENU_FIELD_YEAR;
        default:
            return CLOCK_MENU_FIELD_IDLE;
    }
}

uint8_t clock_menu_toggle_format(uint8_t format)
{
    return format == 0 ? 1 : 0;
}
