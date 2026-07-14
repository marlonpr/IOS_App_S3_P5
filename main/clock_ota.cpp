#include "clock_ota.h"

#include <atomic>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "clock_ota_core.h"

namespace {

constexpr size_t kVersionJsonMaxBytes = 4096;
constexpr int kHttpTimeoutMs = 15000;
constexpr int kMaxRedirects = 8;
constexpr uint32_t kStartupDelayMs = 5000;
constexpr uint32_t kRestartDelayMs = 1000;
constexpr int kOtaTaskStackBytes = 12288;
constexpr UBaseType_t kOtaTaskPriority = 1;
constexpr int kProgressLogIntervalBytes = 64 * 1024;

const char *const TAG = "CLOCK_OTA";

std::atomic<bool> s_github_check_started{false};

struct VersionDownloadContext {
    char data[kVersionJsonMaxBytes + 1];
    size_t stored_bytes;
    size_t total_bytes;
    bool overflow;
};

bool basic_app_self_test()
{
    const esp_app_desc_t *description = esp_app_get_description();
    return description != nullptr && description->version[0] != '\0';
}

esp_err_t version_http_event_handler(esp_http_client_event_t *event)
{
    if (event == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto *context = static_cast<VersionDownloadContext *>(event->user_data);

    switch (event->event_id) {
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG,
                     "version.json redirect (HTTP %d)",
                     esp_http_client_get_status_code(event->client));
            if (context != nullptr) {
                context->stored_bytes = 0;
                context->total_bytes = 0;
                context->overflow = false;
                context->data[0] = '\0';
            }
            break;

        case HTTP_EVENT_ON_DATA:
            if (context == nullptr || event->data == nullptr || event->data_len <= 0) {
                break;
            }

            context->total_bytes += static_cast<size_t>(event->data_len);

            if (context->stored_bytes + static_cast<size_t>(event->data_len) >
                kVersionJsonMaxBytes) {
                context->overflow = true;
                break;
            }

            memcpy(context->data + context->stored_bytes,
                   event->data,
                   static_cast<size_t>(event->data_len));
            context->stored_bytes += static_cast<size_t>(event->data_len);
            context->data[context->stored_bytes] = '\0';
            break;

        default:
            break;
    }

    return ESP_OK;
}

bool download_version_manifest(clock_ota_manifest_t *manifest)
{
    if (manifest == nullptr) {
        return false;
    }

    VersionDownloadContext context = {};
    esp_http_client_config_t config = {};
    config.url = CLOCK_OTA_VERSION_JSON_URL;
    config.user_agent = "ESP32-S3-Clock-OTA/1";
    config.timeout_ms = kHttpTimeoutMs;
    config.disable_auto_redirect = false;
    config.max_redirection_count = kMaxRedirects;
    config.event_handler = version_http_event_handler;
    config.user_data = &context;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = true;

    ESP_LOGI(TAG, "Downloading version manifest: %s", CLOCK_OTA_VERSION_JSON_URL);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize version.json HTTP client");
        return false;
    }

    esp_err_t perform_result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);

    ESP_LOGI(TAG, "version.json HTTP status: %d", status);
    if (content_length >= 0) {
        ESP_LOGI(TAG, "version.json content length: %" PRId64, content_length);
    } else {
        ESP_LOGI(TAG, "version.json content length: unavailable (chunked response)");
    }
    ESP_LOGI(TAG, "version.json bytes read: %u",
             static_cast<unsigned>(context.total_bytes));

    esp_http_client_cleanup(client);

    if (perform_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "version.json download failed: %s",
                 esp_err_to_name(perform_result));
        return false;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "version.json request returned HTTP %d", status);
        return false;
    }

    if (context.overflow) {
        ESP_LOGE(TAG,
                 "version.json exceeds maximum size of %u bytes",
                 static_cast<unsigned>(kVersionJsonMaxBytes));
        return false;
    }

    if (context.stored_bytes == 0) {
        ESP_LOGE(TAG, "version.json response was empty");
        return false;
    }

    if (!clock_ota_parse_version_json(context.data,
                                      context.stored_bytes,
                                      manifest)) {
        ESP_LOGE(TAG,
                 "version.json parse failed; version and url must be unique, non-empty strings");
        return false;
    }

    if (!clock_ota_url_is_supported(manifest->url)) {
        ESP_LOGE(TAG, "version.json contains an unsupported firmware URL");
        return false;
    }

    return true;
}

esp_err_t firmware_http_event_handler(esp_http_client_event_t *event)
{
    if (event != nullptr && event->event_id == HTTP_EVENT_REDIRECT) {
        ESP_LOGI(TAG,
                 "Firmware redirect (HTTP %d)",
                 esp_http_client_get_status_code(event->client));
    }
    return ESP_OK;
}

bool perform_firmware_ota(const char *url, const char *expected_version)
{
    esp_http_client_config_t http_config = {};
    http_config.url = url;
    http_config.user_agent = "ESP32-S3-Clock-OTA/1";
    http_config.timeout_ms = kHttpTimeoutMs;
    http_config.disable_auto_redirect = false;
    http_config.max_redirection_count = kMaxRedirects;
    http_config.event_handler = firmware_http_event_handler;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.keep_alive_enable = true;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    ESP_LOGI(TAG, "Starting firmware OTA download: %s", url);

    esp_https_ota_handle_t ota_handle = nullptr;
    esp_err_t result = esp_https_ota_begin(&ota_config, &ota_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(result));
        return false;
    }

    int status = esp_https_ota_get_status_code(ota_handle);
    int image_size = esp_https_ota_get_image_size(ota_handle);

    ESP_LOGI(TAG, "Firmware HTTP status: %d", status);
    if (image_size >= 0) {
        ESP_LOGI(TAG, "Firmware content length: %d", image_size);
    } else {
        ESP_LOGI(TAG, "Firmware content length: unavailable (chunked response)");
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Firmware request returned HTTP %d", status);
        esp_https_ota_abort(ota_handle);
        return false;
    }

    esp_app_desc_t new_app_info = {};
    result = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to read downloaded firmware description: %s",
                 esp_err_to_name(result));
        esp_https_ota_abort(ota_handle);
        return false;
    }

    ESP_LOGI(TAG, "Downloaded firmware app version: %s", new_app_info.version);

    if (expected_version != nullptr &&
        !clock_ota_versions_equal(new_app_info.version, expected_version)) {
        ESP_LOGE(TAG,
                 "Downloaded firmware version '%s' does not match version.json '%s'",
                 new_app_info.version,
                 expected_version);
        esp_https_ota_abort(ota_handle);
        return false;
    }

    const esp_app_desc_t *current_app_info = esp_app_get_description();
    if (current_app_info != nullptr &&
        clock_ota_versions_equal(new_app_info.version,
                                 current_app_info->version)) {
        ESP_LOGW(TAG,
                 "Downloaded image version '%s' is already running; OTA cancelled",
                 new_app_info.version);
        esp_https_ota_abort(ota_handle);
        return false;
    }

    int last_progress_log = 0;
    do {
        result = esp_https_ota_perform(ota_handle);

        int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
        if (bytes_read >= 0 &&
            (bytes_read - last_progress_log >= kProgressLogIntervalBytes)) {
            ESP_LOGI(TAG, "Firmware bytes read: %d", bytes_read);
            last_progress_log = bytes_read;
        }
    } while (result == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    int final_bytes_read = esp_https_ota_get_image_len_read(ota_handle);
    ESP_LOGI(TAG, "Firmware bytes read: %d", final_bytes_read);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(result));
        esp_https_ota_abort(ota_handle);
        return false;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA download ended before the complete image was received");
        esp_https_ota_abort(ota_handle);
        return false;
    }

    result = esp_https_ota_finish(ota_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation/finalization failed: %s",
                 esp_err_to_name(result));
        return false;
    }

    ESP_LOGI(TAG, "OTA firmware update completed successfully");
    return true;
}

void finish_ota_task(bool update_succeeded)
{
    clock_ota_release_start();

    if (update_succeeded) {
        ESP_LOGI(TAG, "Rebooting into the updated firmware");
        vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
        esp_restart();
    }

    vTaskDelete(nullptr);
}

void github_check_task(void *parameter)
{
    (void)parameter;

    ESP_LOGI(TAG, "Waiting %u ms before the startup OTA check",
             static_cast<unsigned>(kStartupDelayMs));
    vTaskDelay(pdMS_TO_TICKS(kStartupDelayMs));

    clock_ota_manifest_t manifest = {};
    if (!download_version_manifest(&manifest)) {
        finish_ota_task(false);
        return;
    }

    const esp_app_desc_t *description = esp_app_get_description();
    if (description == nullptr) {
        ESP_LOGE(TAG, "Unable to read the current firmware version");
        finish_ota_task(false);
        return;
    }

    ESP_LOGI(TAG, "Current firmware version: %s", description->version);
    ESP_LOGI(TAG, "GitHub firmware version: %s", manifest.version);

    if (clock_ota_versions_equal(description->version, manifest.version)) {
        ESP_LOGI(TAG, "Firmware is already up to date");
        finish_ota_task(false);
        return;
    }

    ESP_LOGI(TAG, "Firmware URL: %s", manifest.url);
    finish_ota_task(perform_firmware_ota(manifest.url, manifest.version));
}

void url_ota_task(void *parameter)
{
    char *task_url = static_cast<char *>(parameter);
    bool update_succeeded = perform_firmware_ota(task_url, nullptr);

    free(task_url);
    finish_ota_task(update_succeeded);
}

}  // namespace

void clock_ota_confirm_app_if_needed(void)
{
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition == nullptr) {
        ESP_LOGE(TAG, "Rollback state unavailable: no running app partition");
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t result = esp_ota_get_state_partition(running_partition, &state);

    if (result == ESP_ERR_NOT_FOUND || result == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG,
                 "No pending OTA verification for running partition '%s'",
                 running_partition->label);
        return;
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Could not read OTA rollback state for '%s': %s",
                 running_partition->label,
                 esp_err_to_name(result));
        return;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG,
                 "Running app rollback state is %d; no confirmation needed",
                 static_cast<int>(state));
        return;
    }

    ESP_LOGW(TAG, "Running OTA app is pending verify");

    if (!basic_app_self_test()) {
        ESP_LOGE(TAG, "Basic app self-test failed; starting rollback path");
        esp_err_t rollback_result = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG,
                 "Rollback request returned unexpectedly: %s",
                 esp_err_to_name(rollback_result));
        return;
    }

    result = esp_ota_mark_app_valid_cancel_rollback();
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "OTA app marked valid; rollback cancelled");
    } else {
        ESP_LOGE(TAG, "Failed to mark OTA app valid: %s", esp_err_to_name(result));
    }
}

void clock_ota_print_app_info(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    if (description == nullptr) {
        ESP_LOGE(TAG, "Application description is unavailable");
        return;
    }

    ESP_LOGI(TAG, "Project: %s", description->project_name);
    ESP_LOGI(TAG, "Firmware version: %s", description->version);
    ESP_LOGI(TAG, "ESP-IDF version: %s", description->idf_ver);

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition != nullptr) {
        ESP_LOGI(TAG,
                 "Running partition: %s at 0x%08" PRIx32 " (%" PRIu32 " bytes)",
                 running_partition->label,
                 running_partition->address,
                 running_partition->size);
    }
}

esp_err_t clock_ota_start_github_check_once(void)
{
    bool expected = false;
    if (!s_github_check_started.compare_exchange_strong(expected, true)) {
        ESP_LOGI(TAG, "Startup GitHub OTA check already launched this boot");
        return ESP_ERR_INVALID_STATE;
    }

    clock_ota_start_result_t reserve_result =
        clock_ota_try_reserve_start(CLOCK_OTA_VERSION_JSON_URL);

    if (reserve_result != CLOCK_OTA_START_ACCEPTED) {
        ESP_LOGW(TAG, "Could not reserve startup OTA check (reason=%d)",
                 static_cast<int>(reserve_result));
        return reserve_result == CLOCK_OTA_START_INVALID_URL
                   ? ESP_ERR_INVALID_ARG
                   : ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_result = xTaskCreate(github_check_task,
                                         "GithubOtaCheck",
                                         kOtaTaskStackBytes,
                                         nullptr,
                                         kOtaTaskPriority,
                                         nullptr);
    if (task_result != pdPASS) {
        clock_ota_release_start();
        ESP_LOGE(TAG, "Failed to create startup OTA check task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Startup GitHub OTA check task launched");
    return ESP_OK;
}

esp_err_t clock_ota_start_from_url(const char *url)
{
    clock_ota_start_result_t reserve_result = clock_ota_try_reserve_start(url);
    if (reserve_result == CLOCK_OTA_START_INVALID_URL) {
        ESP_LOGE(TAG, "OTA URL must be non-empty and start with http:// or https://");
        return ESP_ERR_INVALID_ARG;
    }
    if (reserve_result == CLOCK_OTA_START_ALREADY_IN_PROGRESS) {
        ESP_LOGW(TAG, "Rejecting OTA start because another OTA operation is in progress");
        return ESP_ERR_INVALID_STATE;
    }

    char *task_url = strdup(url);
    if (task_url == nullptr) {
        clock_ota_release_start();
        ESP_LOGE(TAG, "Failed to copy OTA URL");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result = xTaskCreate(url_ota_task,
                                         "UrlOtaTask",
                                         kOtaTaskStackBytes,
                                         task_url,
                                         kOtaTaskPriority,
                                         nullptr);
    if (task_result != pdPASS) {
        free(task_url);
        clock_ota_release_start();
        ESP_LOGE(TAG, "Failed to create URL OTA task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "URL OTA task launched");
    return ESP_OK;
}
