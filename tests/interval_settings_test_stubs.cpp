#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

struct Handle {
    std::string namespace_name;
    nvs_open_mode_t mode;
    std::map<std::string, std::vector<uint8_t>> pending_sets;
    std::set<std::string> pending_erases;
};

std::map<std::string, std::map<std::string, std::vector<uint8_t>>> s_storage;
std::map<nvs_handle_t, Handle> s_handles;
nvs_handle_t s_next_handle = 1;
esp_err_t s_commit_result = ESP_OK;
int s_set_count = 0;
int s_commit_count = 0;
int64_t s_now_us = 1000000;
int s_scroll_stop_count = 0;

std::vector<uint8_t> encode_u16(uint16_t value)
{
    return {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>(value >> 8),
    };
}

esp_err_t read_value(nvs_handle_t handle,
                     const char *key,
                     std::vector<uint8_t> *out_value)
{
    const auto handle_it = s_handles.find(handle);
    if (handle_it == s_handles.end() || key == nullptr || out_value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const auto namespace_it = s_storage.find(handle_it->second.namespace_name);
    if (namespace_it == s_storage.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    const auto value_it = namespace_it->second.find(key);
    if (value_it == namespace_it->second.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    *out_value = value_it->second;
    return ESP_OK;
}

esp_err_t stage_value(nvs_handle_t handle,
                      const char *key,
                      const std::vector<uint8_t> &value)
{
    const auto handle_it = s_handles.find(handle);
    if (handle_it == s_handles.end() ||
        handle_it->second.mode != NVS_READWRITE || key == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    handle_it->second.pending_sets[key] = value;
    handle_it->second.pending_erases.erase(key);
    s_set_count++;
    return ESP_OK;
}

}  // namespace

void interval_test_reset_environment(void)
{
    s_storage.clear();
    s_handles.clear();
    s_next_handle = 1;
    s_commit_result = ESP_OK;
    s_set_count = 0;
    s_commit_count = 0;
    s_now_us = 1000000;
    s_scroll_stop_count = 0;
}

void interval_test_reset_counters(void)
{
    s_set_count = 0;
    s_commit_count = 0;
    s_scroll_stop_count = 0;
}

void interval_test_store_u16(const char *key, uint16_t value)
{
    s_storage["clock_cfg"][key] = encode_u16(value);
}

void interval_test_store_corrupt(const char *key)
{
    s_storage["clock_cfg"][key] = {0xff};
}

void interval_test_set_commit_result(esp_err_t result)
{
    s_commit_result = result;
}

int interval_test_set_count(void)
{
    return s_set_count;
}

int interval_test_commit_count(void)
{
    return s_commit_count;
}

void interval_test_set_time_us(int64_t now_us)
{
    s_now_us = now_us;
}

void interval_test_advance_time_us(int64_t delta_us)
{
    s_now_us += delta_us;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void scroll_stop(void)
{
    s_scroll_stop_count++;
}

const char *esp_err_to_name(esp_err_t result)
{
    switch (result) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_FAIL:
            return "ESP_FAIL";
        case ESP_ERR_INVALID_ARG:
            return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_NVS_NOT_FOUND:
            return "ESP_ERR_NVS_NOT_FOUND";
        case ESP_ERR_NVS_INVALID_LENGTH:
            return "ESP_ERR_NVS_INVALID_LENGTH";
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
    interval_test_reset_environment();
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name,
                   nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    if (namespace_name == nullptr || out_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (open_mode == NVS_READONLY &&
        s_storage.find(namespace_name) == s_storage.end()) {
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
    std::vector<uint8_t> value;
    const esp_err_t result = read_value(handle, key, &value);
    if (result != ESP_OK) {
        return result;
    }
    if (value.size() != 1 || out_value == nullptr) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    *out_value = value[0];
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle,
                     const char *key,
                     unsigned char value)
{
    return stage_value(handle, key, {value});
}

esp_err_t nvs_get_u16(nvs_handle_t handle,
                      const char *key,
                      unsigned short *out_value)
{
    std::vector<uint8_t> value;
    const esp_err_t result = read_value(handle, key, &value);
    if (result != ESP_OK) {
        return result;
    }
    if (value.size() != 2 || out_value == nullptr) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    *out_value = static_cast<unsigned short>(
        value[0] | (static_cast<unsigned short>(value[1]) << 8));
    return ESP_OK;
}

esp_err_t nvs_set_u16(nvs_handle_t handle,
                      const char *key,
                      unsigned short value)
{
    return stage_value(handle, key, encode_u16(value));
}

esp_err_t nvs_get_blob(nvs_handle_t handle,
                       const char *key,
                       void *out_value,
                       size_t *length)
{
    if (length == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::vector<uint8_t> value;
    const esp_err_t result = read_value(handle, key, &value);
    if (result != ESP_OK) {
        return result;
    }
    if (out_value == nullptr) {
        *length = value.size();
        return ESP_OK;
    }
    if (*length < value.size()) {
        *length = value.size();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    std::memcpy(out_value, value.data(), value.size());
    *length = value.size();
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle,
                       const char *key,
                       const void *value,
                       size_t length)
{
    if (value == nullptr || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto *bytes = static_cast<const uint8_t *>(value);
    return stage_value(handle,
                       key,
                       std::vector<uint8_t>(bytes, bytes + length));
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    const auto handle_it = s_handles.find(handle);
    if (handle_it == s_handles.end() || key == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    handle_it->second.pending_sets.erase(key);
    handle_it->second.pending_erases.insert(key);
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    const auto handle_it = s_handles.find(handle);
    if (handle_it == s_handles.end()) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const auto &entry : s_storage[handle_it->second.namespace_name]) {
        handle_it->second.pending_erases.insert(entry.first);
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    const auto handle_it = s_handles.find(handle);
    if (handle_it == s_handles.end()) {
        return ESP_ERR_INVALID_ARG;
    }

    s_commit_count++;
    if (s_commit_result != ESP_OK) {
        return s_commit_result;
    }

    auto &name_space = s_storage[handle_it->second.namespace_name];
    for (const auto &key : handle_it->second.pending_erases) {
        name_space.erase(key);
    }
    for (const auto &entry : handle_it->second.pending_sets) {
        name_space[entry.first] = entry.second;
    }
    return ESP_OK;
}
