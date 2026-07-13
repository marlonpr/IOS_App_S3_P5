#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "esp_err.h"
#include "nvs.h"

struct FakeHandle {
    std::string namespace_name;
    nvs_open_mode_t mode = NVS_READONLY;
    bool erase_all = false;
    std::map<std::string, std::vector<uint8_t>> pending_sets;
    std::set<std::string> pending_erases;
};

static std::map<std::string, std::map<std::string, std::vector<uint8_t>>>
    s_namespaces;
static std::map<nvs_handle_t, FakeHandle> s_handles;
static nvs_handle_t s_next_handle = 1;
static esp_err_t s_commit_result = ESP_OK;
static int s_set_count = 0;
static int s_commit_count = 0;
static int s_erase_key_count = 0;
static int s_erase_all_count = 0;

void palette_test_reset_nvs(void)
{
    s_namespaces.clear();
    s_handles.clear();
    s_next_handle = 1;
    s_commit_result = ESP_OK;
    s_set_count = 0;
    s_commit_count = 0;
    s_erase_key_count = 0;
    s_erase_all_count = 0;
}

void palette_test_reset_counters(void)
{
    s_set_count = 0;
    s_commit_count = 0;
    s_erase_key_count = 0;
    s_erase_all_count = 0;
}

void palette_test_set_commit_result(esp_err_t result)
{
    s_commit_result = result;
}

int palette_test_set_count(void)
{
    return s_set_count;
}

int palette_test_commit_count(void)
{
    return s_commit_count;
}

int palette_test_erase_key_count(void)
{
    return s_erase_key_count;
}

int palette_test_erase_all_count(void)
{
    return s_erase_all_count;
}

void palette_test_store_raw_blob(const char *namespace_name,
                                 const char *key,
                                 const std::vector<uint8_t> &blob)
{
    s_namespaces[namespace_name][key] = blob;
}

bool palette_test_read_raw_blob(const char *namespace_name,
                                const char *key,
                                std::vector<uint8_t> *out_blob)
{
    auto namespace_it = s_namespaces.find(namespace_name);

    if (namespace_it == s_namespaces.end()) {
        return false;
    }

    auto value_it = namespace_it->second.find(key);

    if (value_it == namespace_it->second.end()) {
        return false;
    }

    if (out_blob != nullptr) {
        *out_blob = value_it->second;
    }

    return true;
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_FAIL:
            return "ESP_FAIL";
        case ESP_ERR_INVALID_ARG:
            return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_SIZE:
            return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_INVALID_STATE:
            return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NVS_NOT_FOUND:
            return "ESP_ERR_NVS_NOT_FOUND";
        case ESP_ERR_NVS_INVALID_LENGTH:
            return "ESP_ERR_NVS_INVALID_LENGTH";
        default:
            return "ESP_ERR_UNKNOWN";
    }
}

esp_err_t nvs_open(const char *namespace_name,
                   nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    if (namespace_name == nullptr || out_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (open_mode == NVS_READONLY &&
        s_namespaces.find(namespace_name) == s_namespaces.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    nvs_handle_t handle = s_next_handle++;
    s_handles[handle] = {namespace_name, open_mode};
    *out_handle = handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    s_handles.erase(handle);
}

esp_err_t nvs_get_blob(nvs_handle_t handle,
                       const char *key,
                       void *out_value,
                       size_t *length)
{
    auto handle_it = s_handles.find(handle);

    if (handle_it == s_handles.end() || key == nullptr || length == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto namespace_it = s_namespaces.find(handle_it->second.namespace_name);

    if (namespace_it == s_namespaces.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    auto value_it = namespace_it->second.find(key);

    if (value_it == namespace_it->second.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    const std::vector<uint8_t> &value = value_it->second;

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
    auto handle_it = s_handles.find(handle);

    if (handle_it == s_handles.end() ||
        handle_it->second.mode != NVS_READWRITE || key == nullptr ||
        value == nullptr || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *bytes = static_cast<const uint8_t *>(value);
    handle_it->second.pending_sets[key] =
        std::vector<uint8_t>(bytes, bytes + length);
    handle_it->second.pending_erases.erase(key);
    s_set_count++;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    auto handle_it = s_handles.find(handle);

    if (handle_it == s_handles.end() ||
        handle_it->second.mode != NVS_READWRITE || key == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    s_erase_key_count++;
    auto namespace_it = s_namespaces.find(handle_it->second.namespace_name);
    bool exists = namespace_it != s_namespaces.end() &&
                  namespace_it->second.find(key) != namespace_it->second.end();

    if (!exists &&
        handle_it->second.pending_sets.find(key) ==
            handle_it->second.pending_sets.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    handle_it->second.pending_sets.erase(key);
    handle_it->second.pending_erases.insert(key);
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    auto handle_it = s_handles.find(handle);

    if (handle_it == s_handles.end() ||
        handle_it->second.mode != NVS_READWRITE) {
        return ESP_ERR_INVALID_ARG;
    }

    handle_it->second.erase_all = true;
    handle_it->second.pending_sets.clear();
    handle_it->second.pending_erases.clear();
    s_erase_all_count++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    auto handle_it = s_handles.find(handle);

    if (handle_it == s_handles.end() ||
        handle_it->second.mode != NVS_READWRITE) {
        return ESP_ERR_INVALID_ARG;
    }

    s_commit_count++;

    if (s_commit_result != ESP_OK) {
        return s_commit_result;
    }

    FakeHandle &fake_handle = handle_it->second;
    auto &target_namespace = s_namespaces[fake_handle.namespace_name];

    if (fake_handle.erase_all) {
        target_namespace.clear();
    }

    for (const std::string &key : fake_handle.pending_erases) {
        target_namespace.erase(key);
    }

    for (const auto &pending_set : fake_handle.pending_sets) {
        target_namespace[pending_set.first] = pending_set.second;
    }

    fake_handle.erase_all = false;
    fake_handle.pending_erases.clear();
    fake_handle.pending_sets.clear();
    return ESP_OK;
}
