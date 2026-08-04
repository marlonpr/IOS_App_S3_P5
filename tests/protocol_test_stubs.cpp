#include <cstring>

#include "clock_alarm.h"
#include "clock_logo_manager.h"
#include "clock_menu.h"
#include "clock_modes.h"

static ethernet_alarm_t s_alarms[MAX_ETH_ALARMS] = {};
static uint8_t s_display_mode = 1;
static int s_mode_save_count = 0;
static int s_mode_reset_count = 0;
static int s_mode_advance_count = 0;
static esp_err_t s_mode_set_result = ESP_OK;
static clock_mode_intervals_t s_intervals = {
    CLOCK_DEFAULT_LOGO_INTERVAL_SECONDS,
    CLOCK_DEFAULT_MODE_INTERVAL_SECONDS,
};
static int s_interval_save_count = 0;
static int s_interval_restore_count = 0;
static esp_err_t s_interval_save_result = ESP_OK;

void protocol_test_reset_alarms(void)
{
    std::memset(s_alarms, 0, sizeof(s_alarms));
}

void protocol_test_reset_modes(void)
{
    s_display_mode = 1;
    s_mode_save_count = 0;
    s_mode_reset_count = 0;
    s_mode_advance_count = 0;
    s_mode_set_result = ESP_OK;
    s_intervals = {
        CLOCK_DEFAULT_LOGO_INTERVAL_SECONDS,
        CLOCK_DEFAULT_MODE_INTERVAL_SECONDS,
    };
    s_interval_save_count = 0;
    s_interval_restore_count = 0;
    s_interval_save_result = ESP_OK;
}

uint8_t protocol_test_display_mode(void)
{
    return s_display_mode;
}

int protocol_test_mode_save_count(void)
{
    return s_mode_save_count;
}

int protocol_test_mode_reset_count(void)
{
    return s_mode_reset_count;
}

int protocol_test_mode_advance_count(void)
{
    return s_mode_advance_count;
}

void protocol_test_set_mode_result(esp_err_t result)
{
    s_mode_set_result = result;
}

void protocol_test_force_display_mode(uint8_t mode)
{
    s_display_mode = mode;
}

clock_mode_intervals_t protocol_test_intervals(void)
{
    return s_intervals;
}

int protocol_test_interval_save_count(void)
{
    return s_interval_save_count;
}

int protocol_test_interval_restore_count(void)
{
    return s_interval_restore_count;
}

void protocol_test_set_interval_save_result(esp_err_t result)
{
    s_interval_save_result = result;
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
    s_mode_reset_count++;
}

uint8_t clock_modes_advance_mode(void)
{
    s_mode_advance_count++;
    s_display_mode = s_display_mode >= 4 ? 1 : s_display_mode + 1;
    s_mode_save_count++;
    clock_modes_reset_sequences();
    return s_display_mode;
}

esp_err_t clock_modes_set_mode(uint8_t mode)
{
    if (mode < 1 || mode > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    s_display_mode = mode;
    s_mode_save_count++;
    clock_modes_reset_sequences();
    return s_mode_set_result;
}

uint8_t clock_modes_get_mode(void)
{
    return s_display_mode;
}

clock_mode_intervals_t clock_modes_get_intervals(void)
{
    return s_intervals;
}

bool clock_modes_validate_intervals(const clock_mode_intervals_t *intervals)
{
    return intervals != nullptr &&
           intervals->logo_seconds >= CLOCK_MODE_INTERVAL_MIN_SECONDS &&
           intervals->logo_seconds <= CLOCK_MODE_INTERVAL_MAX_SECONDS &&
           intervals->mode_seconds >= CLOCK_MODE_INTERVAL_MIN_SECONDS &&
           intervals->mode_seconds <= CLOCK_MODE_INTERVAL_MAX_SECONDS;
}

esp_err_t clock_modes_set_intervals(const clock_mode_intervals_t *intervals)
{
    if (!clock_modes_validate_intervals(intervals)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_interval_save_result != ESP_OK) {
        return s_interval_save_result;
    }

    if (s_intervals.logo_seconds != intervals->logo_seconds ||
        s_intervals.mode_seconds != intervals->mode_seconds) {
        s_interval_save_count++;
    }
    s_intervals = *intervals;
    clock_modes_reset_sequences();
    return ESP_OK;
}

esp_err_t clock_modes_restore_default_intervals(void)
{
    if (s_interval_save_result != ESP_OK) {
        return s_interval_save_result;
    }

    s_interval_save_count++;
    s_interval_restore_count++;
    s_intervals = {
        CLOCK_DEFAULT_LOGO_INTERVAL_SECONDS,
        CLOCK_DEFAULT_MODE_INTERVAL_SECONDS,
    };
    clock_modes_reset_sequences();
    return ESP_OK;
}
