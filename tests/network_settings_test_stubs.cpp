#include <cstring>
#include <map>
#include <string>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

struct Handle {
    std::string namespace_name;
    nvs_open_mode_t mode;
};

std::map<std::string, unsigned char> s_values;
std::map<nvs_handle_t, Handle> s_handles;
nvs_handle_t s_next_handle = 1;

std::string storage_key(const Handle &handle, const char *key)
{
    return handle.namespace_name + "/" + key;
}

}  // namespace

void network_settings_test_reset(void)
{
    s_values.clear();
    s_handles.clear();
    s_next_handle = 1;
}

void network_settings_test_store_raw(unsigned char value)
{
    s_values["clock_cfg/network_mode"] = value;
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_ERR_INVALID_ARG:
            return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_NVS_NOT_FOUND:
            return "ESP_ERR_NVS_NOT_FOUND";
        default:
            return "ESP_ERR_UNKNOWN";
    }
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    network_settings_test_reset();
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name,
                   nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    if (namespace_name == nullptr || out_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    bool namespace_exists = false;
    const std::string prefix = std::string(namespace_name) + "/";
    for (const auto &entry : s_values) {
        if (entry.first.compare(0, prefix.size(), prefix) == 0) {
            namespace_exists = true;
            break;
        }
    }

    if (open_mode == NVS_READONLY && !namespace_exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    const nvs_handle_t handle = s_next_handle++;
    s_handles.emplace(handle, Handle{namespace_name, open_mode});
    *out_handle = handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    s_handles.erase(handle);
}

esp_err_t nvs_get_u8(nvs_handle_t handle,
                     const char *key,
                     unsigned char *out_value)
{
    const auto handle_it = s_handles.find(handle);
    if (handle_it == s_handles.end() || key == nullptr || out_value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const auto value_it = s_values.find(storage_key(handle_it->second, key));
    if (value_it == s_values.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    *out_value = value_it->second;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle,
                     const char *key,
                     unsigned char value)
{
    const auto handle_it = s_handles.find(handle);
    if (handle_it == s_handles.end() ||
        handle_it->second.mode != NVS_READWRITE || key == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    s_values[storage_key(handle_it->second, key)] = value;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t,
                       const char *,
                       void *,
                       size_t *)
{
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t,
                       const char *,
                       const void *,
                       size_t)
{
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t, const char *)
{
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t)
{
    s_values.clear();
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t)
{
    return ESP_OK;
}
