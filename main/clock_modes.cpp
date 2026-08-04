#include "clock_modes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "led_panel.h"

#include "clock_settings.h"

static const char *TAG = "CLOCK_MODES";

static clock_mode_intervals_t s_intervals = {
    CLOCK_DEFAULT_LOGO_INTERVAL_SECONDS,
    CLOCK_DEFAULT_MODE_INTERVAL_SECONDS,
};

static rotation_item_t s_rotation_item = ROT_ITEM_LOGO;
static int64_t s_rotation_last_change_us = 0;

static fixed_item_t s_fixed_item = FIXED_ITEM_LOGO;
static int64_t s_fixed_last_change_us = 0;

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_operation_mutex_storage;
static SemaphoreHandle_t s_operation_mutex = nullptr;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static clock_mode_intervals_t default_intervals(void)
{
    return {
        CLOCK_DEFAULT_LOGO_INTERVAL_SECONDS,
        CLOCK_DEFAULT_MODE_INTERVAL_SECONDS,
    };
}

static bool ensure_operation_mutex(void)
{
    if (s_operation_mutex != nullptr) {
        return true;
    }

    portENTER_CRITICAL(&s_init_mux);
    if (s_operation_mutex == nullptr) {
        s_operation_mutex =
            xSemaphoreCreateMutexStatic(&s_operation_mutex_storage);
    }
    portEXIT_CRITICAL(&s_init_mux);

    return s_operation_mutex != nullptr;
}

static bool operation_lock(void)
{
    return ensure_operation_mutex() &&
           xSemaphoreTake(s_operation_mutex, portMAX_DELAY) == pdTRUE;
}

static void operation_unlock(void)
{
    if (s_operation_mutex != nullptr) {
        xSemaphoreGive(s_operation_mutex);
    }
}

static bool intervals_equal(const clock_mode_intervals_t *left,
                            const clock_mode_intervals_t *right)
{
    return left != nullptr && right != nullptr &&
           left->logo_seconds == right->logo_seconds &&
           left->mode_seconds == right->mode_seconds;
}

static void replace_intervals_and_reset(
    const clock_mode_intervals_t *intervals)
{
    portENTER_CRITICAL(&s_state_mux);
    s_intervals = *intervals;
    s_fixed_last_change_us = 0;
    s_fixed_item = FIXED_ITEM_LOGO;
    s_rotation_last_change_us = 0;
    s_rotation_item = ROT_ITEM_LOGO;
    portEXIT_CRITICAL(&s_state_mux);

    scroll_stop();
}

bool clock_modes_validate_intervals(const clock_mode_intervals_t *intervals)
{
    if (intervals == nullptr) {
        return false;
    }

    return intervals->logo_seconds >= CLOCK_MODE_INTERVAL_MIN_SECONDS &&
           intervals->logo_seconds <= CLOCK_MODE_INTERVAL_MAX_SECONDS &&
           intervals->mode_seconds >= CLOCK_MODE_INTERVAL_MIN_SECONDS &&
           intervals->mode_seconds <= CLOCK_MODE_INTERVAL_MAX_SECONDS;
}

clock_mode_intervals_t clock_modes_get_intervals(void)
{
    clock_mode_intervals_t snapshot;

    portENTER_CRITICAL(&s_state_mux);
    snapshot = s_intervals;
    portEXIT_CRITICAL(&s_state_mux);

    return snapshot;
}

esp_err_t clock_modes_init_intervals(void)
{
    if (!operation_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    clock_mode_intervals_t active = default_intervals();
    clock_mode_intervals_t stored = {};
    const esp_err_t load_result = clock_settings_load_display_intervals(
        &stored.logo_seconds,
        &stored.mode_seconds);

    if (load_result == ESP_OK && clock_modes_validate_intervals(&stored)) {
        active = stored;
    } else if (load_result == ESP_OK) {
        ESP_LOGW(TAG,
                 "Invalid display interval pair in NVS; using defaults");
    }

    replace_intervals_and_reset(&active);
    operation_unlock();

    ESP_LOGI(TAG,
             "Display intervals loaded: logo=%u s mode=%u s",
             active.logo_seconds,
             active.mode_seconds);
    return ESP_OK;
}

esp_err_t clock_modes_set_intervals(const clock_mode_intervals_t *intervals)
{
    if (!clock_modes_validate_intervals(intervals)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!operation_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    const clock_mode_intervals_t current = clock_modes_get_intervals();
    esp_err_t result = ESP_OK;

    if (!intervals_equal(&current, intervals)) {
        result = clock_settings_save_display_intervals(
            intervals->logo_seconds,
            intervals->mode_seconds);
    }

    if (result == ESP_OK) {
        replace_intervals_and_reset(intervals);
    }

    operation_unlock();
    return result;
}

esp_err_t clock_modes_restore_default_intervals(void)
{
    const clock_mode_intervals_t defaults = default_intervals();

    if (!operation_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = clock_settings_save_display_intervals(
        defaults.logo_seconds,
        defaults.mode_seconds);

    if (result == ESP_OK) {
        replace_intervals_and_reset(&defaults);
    }

    operation_unlock();
    return result;
}

rotation_item_t clock_modes_get_rotation_item(void)
{
    const int64_t now_us = esp_timer_get_time();
    bool item_changed = false;

    portENTER_CRITICAL(&s_state_mux);
    const clock_mode_intervals_t intervals = s_intervals;

    if (s_rotation_last_change_us == 0) {
        s_rotation_last_change_us = now_us;
        s_rotation_item = ROT_ITEM_LOGO;
        const rotation_item_t item = s_rotation_item;
        portEXIT_CRITICAL(&s_state_mux);
        return item;
    }

    const uint16_t seconds = s_rotation_item == ROT_ITEM_LOGO
                                 ? intervals.logo_seconds
                                 : intervals.mode_seconds;
    const int64_t interval_us = (int64_t)seconds * 1000000LL;

    if ((now_us - s_rotation_last_change_us) >= interval_us) {
        s_rotation_last_change_us = now_us;

        switch (s_rotation_item)
        {
            case ROT_ITEM_LOGO:
                s_rotation_item = ROT_ITEM_MODE_1;
                break;

            case ROT_ITEM_MODE_1:
                s_rotation_item = ROT_ITEM_MODE_2;
                break;

            case ROT_ITEM_MODE_2:
                s_rotation_item = ROT_ITEM_MODE_3;
                break;

            case ROT_ITEM_MODE_3:
            default:
                s_rotation_item = ROT_ITEM_LOGO;
                break;
        }
        item_changed = true;
    }

    const rotation_item_t item = s_rotation_item;
    portEXIT_CRITICAL(&s_state_mux);

    if (item_changed) {
        scroll_stop();
    }

    return item;
}

fixed_item_t clock_modes_get_fixed_item(void)
{
    const int64_t now_us = esp_timer_get_time();
    bool item_changed = false;

    portENTER_CRITICAL(&s_state_mux);
    const clock_mode_intervals_t intervals = s_intervals;

    if (s_fixed_last_change_us == 0) {
        s_fixed_last_change_us = now_us;
        s_fixed_item = FIXED_ITEM_LOGO;
        const fixed_item_t item = s_fixed_item;
        portEXIT_CRITICAL(&s_state_mux);
        return item;
    }

    const uint16_t seconds = s_fixed_item == FIXED_ITEM_LOGO
                                 ? intervals.logo_seconds
                                 : intervals.mode_seconds;
    const int64_t interval_us = (int64_t)seconds * 1000000LL;

    if ((now_us - s_fixed_last_change_us) >= interval_us) {
        s_fixed_last_change_us = now_us;

        if (s_fixed_item == FIXED_ITEM_LOGO) {
            s_fixed_item = FIXED_ITEM_SCREEN;
        } else {
            s_fixed_item = FIXED_ITEM_LOGO;
        }

        item_changed = true;
    }

    const fixed_item_t item = s_fixed_item;
    portEXIT_CRITICAL(&s_state_mux);

    if (item_changed) {
        scroll_stop();
    }

    return item;
}

void clock_modes_reset_sequences(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_fixed_last_change_us = 0;
    s_fixed_item = FIXED_ITEM_LOGO;
    s_rotation_last_change_us = 0;
    s_rotation_item = ROT_ITEM_LOGO;
    portEXIT_CRITICAL(&s_state_mux);

    scroll_stop();
}
