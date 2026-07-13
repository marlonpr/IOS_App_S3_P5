#include <cstring>

#include "clock_alarm.h"
#include "clock_logo_manager.h"
#include "clock_menu.h"
#include "clock_modes.h"

static ethernet_alarm_t s_alarms[MAX_ETH_ALARMS] = {};

void protocol_test_reset_alarms(void)
{
    std::memset(s_alarms, 0, sizeof(s_alarms));
}

void clock_alarm_arm_full_replacement(void)
{
}

esp_err_t clock_alarm_store_from_ca(uint8_t alarm_id,
                                    uint8_t alarm_hh,
                                    uint8_t alarm_mm,
                                    uint8_t frequency,
                                    uint8_t duration_effect)
{
    if (alarm_id < 1 || alarm_id > MAX_ETH_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    ethernet_alarm_t &alarm = s_alarms[alarm_id - 1];
    alarm.configured = true;
    alarm.alarm_id = alarm_id;
    alarm.time_hh = alarm_hh;
    alarm.time_mm = alarm_mm;
    alarm.frequency = frequency;
    alarm.duration_effect = duration_effect;
    return ESP_OK;
}

esp_err_t clock_alarm_delete_from_da(uint8_t alarm_id)
{
    if (alarm_id < 1 || alarm_id > MAX_ETH_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    std::memset(&s_alarms[alarm_id - 1], 0, sizeof(ethernet_alarm_t));
    return ESP_OK;
}

bool clock_alarm_read(uint8_t alarm_id, ethernet_alarm_t *out_alarm)
{
    if (alarm_id < 1 || alarm_id > MAX_ETH_ALARMS || out_alarm == nullptr) {
        return false;
    }

    *out_alarm = s_alarms[alarm_id - 1];
    out_alarm->alarm_id = alarm_id;
    return true;
}

extern "C" int clock_menu_calculate_weekday(int day, int month, int year)
{
    (void)day;
    (void)month;
    (void)year;
    return 1;
}

extern "C" int clock_menu_days_in_month(int month, int year)
{
    (void)year;

    if (month == 2) {
        return 28;
    }

    if (month == 4 || month == 6 || month == 9 || month == 11) {
        return 30;
    }

    return 31;
}

extern "C" clock_logo_restore_result_t clock_logo_restore_compiled_default(void)
{
    return CLOCK_LOGO_RESTORE_OK;
}

void clock_modes_reset_sequences(void)
{
}

uint8_t clock_modes_advance_mode(void)
{
    return 1;
}
