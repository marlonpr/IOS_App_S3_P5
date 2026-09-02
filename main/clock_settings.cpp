#include "clock_settings.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "clock_network.h"

static const char *TAG = "CLOCK_SETTINGS";

#define NVS_NAMESPACE      "clock_cfg"
#define NVS_KEY_FORMAT     "format"
#define NVS_KEY_MODE       "mode"
#define NVS_KEY_BRIGHTNESS "brightness"
#define NVS_KEY_NETWORK_MODE "network_mode"
#define NVS_KEY_LOGO_SECONDS "logo_sec"
#define NVS_KEY_MODE_SECONDS "mode_sec"

#define NVS_KEY_ETH_ALARMS "eth_alarms"



esp_err_t clock_settings_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t save_u8_value(const char *key, uint8_t value)
{
    nvs_handle_t handle;

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for key '%s': %s",
                 key,
                 esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u8(handle, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Saved %s=%u", key, value);
    } else {
        ESP_LOGE(TAG, "Save failed for key '%s': %s",
                 key,
                 esp_err_to_name(ret));
    }

    return ret;
}

static uint8_t load_u8_value(const char *key, uint8_t default_value)
{
    nvs_handle_t handle;
    uint8_t value = default_value;

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No NVS namespace, using default %s=%u",
                 key,
                 default_value);
        return default_value;
    }

    ret = nvs_get_u8(handle, key, &value);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Key '%s' not found, using default %u",
                 key,
                 default_value);
        return default_value;
    }

    ESP_LOGI(TAG, "Loaded %s=%u", key, value);

    return value;
}

esp_err_t clock_settings_save_display_intervals(uint16_t logo_seconds,
                                                uint16_t mode_seconds)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "NVS open failed for display intervals: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u16(handle, NVS_KEY_LOGO_SECONDS, logo_seconds);
    if (ret == ESP_OK) {
        ret = nvs_set_u16(handle, NVS_KEY_MODE_SECONDS, mode_seconds);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "Saved display intervals: logo=%u s mode=%u s",
                 logo_seconds,
                 mode_seconds);
    } else {
        ESP_LOGE(TAG,
                 "Failed to save display intervals: %s",
                 esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t clock_settings_load_display_intervals(uint16_t *logo_seconds,
                                                uint16_t *mode_seconds)
{
    if (logo_seconds == nullptr || mode_seconds == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Display intervals not stored; using defaults");
        return ret;
    }

    uint16_t stored_logo = 0;
    uint16_t stored_mode = 0;
    ret = nvs_get_u16(handle, NVS_KEY_LOGO_SECONDS, &stored_logo);
    if (ret == ESP_OK) {
        ret = nvs_get_u16(handle, NVS_KEY_MODE_SECONDS, &stored_mode);
    }

    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Incomplete display interval pair; using defaults");
        return ret;
    }

    *logo_seconds = stored_logo;
    *mode_seconds = stored_mode;
    return ESP_OK;
}



static esp_err_t save_blob_value(const char *key, const void *data, size_t size)
{
    if (data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for blob '%s': %s",
                 key,
                 esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, key, data, size);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Saved blob %s, size=%u",
                 key,
                 (unsigned)size);
    } else {
        ESP_LOGE(TAG, "Save blob failed for key '%s': %s",
                 key,
                 esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t load_blob_value(const char *key, void *data, size_t expected_size)
{
    if (data == NULL || expected_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No NVS namespace for blob '%s': %s",
                 key,
                 esp_err_to_name(ret));
        return ret;
    }

    size_t size = expected_size;

    ret = nvs_get_blob(handle, key, data, &size);

    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Blob '%s' not found", key);
        return ret;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Load blob failed for key '%s': %s",
                 key,
                 esp_err_to_name(ret));
        return ret;
    }

    if (size != expected_size) {
        ESP_LOGW(TAG,
                 "Blob '%s' size mismatch: got=%u expected=%u",
                 key,
                 (unsigned)size,
                 (unsigned)expected_size);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Loaded blob %s, size=%u",
             key,
             (unsigned)size);

    return ESP_OK;
}






esp_err_t clock_settings_save_ethernet_alarms(const void *alarms, size_t size)
{
    return save_blob_value(NVS_KEY_ETH_ALARMS, alarms, size);
}

esp_err_t clock_settings_load_ethernet_alarms(void *alarms, size_t size)
{
    return load_blob_value(NVS_KEY_ETH_ALARMS, alarms, size);
}















void clock_settings_save_format(uint8_t format)
{
    (void)save_u8_value(NVS_KEY_FORMAT, format);
}

uint8_t clock_settings_load_format(uint8_t default_format)
{
    return load_u8_value(NVS_KEY_FORMAT, default_format);
}

esp_err_t clock_settings_save_mode(uint8_t mode)
{
    return save_u8_value(NVS_KEY_MODE, mode);
}

uint8_t clock_settings_load_mode(uint8_t default_mode)
{
    return load_u8_value(NVS_KEY_MODE, default_mode);
}

void clock_settings_save_brightness(uint8_t brightness_level)
{
    if (brightness_level < 1) {
        brightness_level = 1;
    }

    if (brightness_level > 10) {
        brightness_level = 10;
    }

    (void)save_u8_value(NVS_KEY_BRIGHTNESS, brightness_level);
}

uint8_t clock_settings_load_brightness(uint8_t default_brightness_level)
{
    uint8_t value = load_u8_value(NVS_KEY_BRIGHTNESS, default_brightness_level);

    if (value < 1 || value > 10) {
        value = default_brightness_level;
    }

    return value;
}

esp_err_t clock_settings_save_network_mode(uint8_t network_mode)
{
    if (network_mode != CLOCK_NETWORK_MODE_ETHERNET &&
        network_mode != CLOCK_NETWORK_MODE_WIFI) {
        ESP_LOGE(TAG, "Rejecting invalid network mode %u", network_mode);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t result =
        save_u8_value(NVS_KEY_NETWORK_MODE, network_mode);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Saved network mode=%u", network_mode);
    }
    return result;
}

uint8_t clock_settings_load_network_mode(void)
{
    constexpr uint8_t kDefaultMode = CLOCK_NETWORK_MODE_WIFI;
    nvs_handle_t handle;
    uint8_t value = kDefaultMode;

    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "No NVS namespace, using default network mode=%u",
                 kDefaultMode);
        return kDefaultMode;
    }

    result = nvs_get_u8(handle, NVS_KEY_NETWORK_MODE, &value);
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Network mode not found, using default network mode=%u",
                 kDefaultMode);
        return kDefaultMode;
    }

    if (value == CLOCK_NETWORK_MODE_ETHERNET ||
        value == CLOCK_NETWORK_MODE_WIFI) {
        ESP_LOGI(TAG, "Loaded network mode=%u", value);
        return value;
    }

    if (value == CLOCK_NETWORK_MODE_LEGACY_AUTO_VALUE) {
        ESP_LOGW(TAG, "Legacy AUTO network mode migrated to Wi-Fi");
    } else {
        ESP_LOGW(TAG,
                 "Invalid network mode %u in NVS; using Wi-Fi",
                 value);
    }

    result = clock_settings_save_network_mode(kDefaultMode);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to persist migrated network mode: %s",
                 esp_err_to_name(result));
    }

    return kDefaultMode;
}
