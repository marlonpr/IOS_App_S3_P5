#include <cstddef>
#include <cstring>

#include "clock_logo_manager.h"
#include "clock_menu.h"
#include "clock_settings.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"

int64_t g_alarm_test_now_us = 0;
int g_alarm_test_save_count = 0;
int g_alarm_test_load_count = 0;
size_t g_alarm_test_last_save_size = 0;

static unsigned char s_alarm_test_last_save_blob[4096] = {};

void alarm_test_reset_stubs(void)
{
    g_alarm_test_now_us = 0;
    g_alarm_test_save_count = 0;
    g_alarm_test_load_count = 0;
    g_alarm_test_last_save_size = 0;
    memset(s_alarm_test_last_save_blob, 0, sizeof(s_alarm_test_last_save_blob));
}

void alarm_test_reset_save_count(void)
{
    g_alarm_test_save_count = 0;
    g_alarm_test_last_save_size = 0;
}

const unsigned char *alarm_test_last_save_blob(void)
{
    return s_alarm_test_last_save_blob;
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_ERR_INVALID_ARG:
            return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_SIZE:
            return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NVS_NOT_FOUND:
            return "ESP_ERR_NVS_NOT_FOUND";
        default:
            return "ESP_ERR_UNKNOWN";
    }
}

int64_t esp_timer_get_time(void)
{
    return g_alarm_test_now_us;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, int level)
{
    (void)gpio_num;
    (void)level;
    return ESP_OK;
}

extern "C" esp_err_t clock_settings_init(void)
{
    return ESP_OK;
}

extern "C" void clock_settings_save_format(uint8_t format)
{
    (void)format;
}

extern "C" uint8_t clock_settings_load_format(uint8_t default_format)
{
    return default_format;
}

extern "C" void clock_settings_save_mode(uint8_t mode)
{
    (void)mode;
}

extern "C" uint8_t clock_settings_load_mode(uint8_t default_mode)
{
    return default_mode;
}

extern "C" void clock_settings_save_brightness(uint8_t brightness_level)
{
    (void)brightness_level;
}

extern "C" uint8_t clock_settings_load_brightness(uint8_t default_brightness_level)
{
    return default_brightness_level;
}

extern "C" esp_err_t clock_settings_save_ethernet_alarms(const void *alarms,
                                                          size_t size)
{
    g_alarm_test_save_count++;
    g_alarm_test_last_save_size = size;

    if (alarms != nullptr && size <= sizeof(s_alarm_test_last_save_blob)) {
        memcpy(s_alarm_test_last_save_blob, alarms, size);
    }

    return ESP_OK;
}

extern "C" esp_err_t clock_settings_load_ethernet_alarms(void *alarms,
                                                          size_t size)
{
    (void)alarms;
    (void)size;
    g_alarm_test_load_count++;
    return ESP_ERR_NVS_NOT_FOUND;
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

    switch (month) {
        case 2:
            return 28;
        case 4:
        case 6:
        case 9:
        case 11:
            return 30;
        default:
            return 31;
    }
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
