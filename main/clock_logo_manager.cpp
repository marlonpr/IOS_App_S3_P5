#include "clock_logo_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "clock_sd_card.h"
#include "logo.h"

static constexpr const char *kBackupLogoPath =
    "/sdcard/logo.bak";

static constexpr const char *kTemporaryLogoPath =
    "/sdcard/logo.tmp";
	
static const char *const kLogoPath = 
	"/sdcard/logo.lgo";

static const char *TAG = "CLOCK_LOGO";


static SemaphoreHandle_t s_logo_operation_mutex = nullptr;
static StaticSemaphore_t s_logo_operation_mutex_storage;

static_assert(LOGO_WIDTH == CLOCK_LOGO_WIDTH, "Compiled logo width must stay 64");
static_assert(LOGO_HEIGHT == CLOCK_LOGO_HEIGHT, "Compiled logo height must stay 32");
static_assert((sizeof(logo_bitmap) / sizeof(logo_bitmap[0])) == CLOCK_LOGO_PIXEL_COUNT,
              "Compiled logo must contain 2048 pixels");

static uint32_t s_logo_buffers[2][CLOCK_LOGO_PIXEL_COUNT] = {};
static const uint32_t *s_active_pixels = logo_bitmap;
static bool s_initialized = false;

static StaticSemaphore_t s_logo_mutex_storage;
static SemaphoreHandle_t s_logo_mutex = nullptr;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void ensure_initialized()
{
    if (s_initialized) {
        return;
    }

    bool initialized_now = false;

    portENTER_CRITICAL(&s_init_mux);

    if (!s_initialized) {
        s_active_pixels = logo_bitmap;

        memset(
            s_logo_buffers[0],
            0,
            sizeof(s_logo_buffers[0])
        );

        memset(
            s_logo_buffers[1],
            0,
            sizeof(s_logo_buffers[1])
        );

        s_logo_mutex =
            xSemaphoreCreateMutexStatic(&s_logo_mutex_storage);

        s_logo_operation_mutex =
            xSemaphoreCreateMutexStatic(
                &s_logo_operation_mutex_storage
            );

        if (s_logo_mutex != nullptr &&
            s_logo_operation_mutex != nullptr) {
            s_initialized = true;
            initialized_now = true;
        }
    }

    portEXIT_CRITICAL(&s_init_mux);

    if (initialized_now) {
        ESP_LOGI(TAG, "Compiled logo fallback active");
    } else if (!s_initialized) {
        ESP_LOGE(TAG, "Failed to initialize logo mutexes");
    }
}

static uint16_t read_le_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

void clock_logo_init()
{
    ensure_initialized();
}

bool clock_logo_operation_begin(uint32_t timeout_ms)
{
    ensure_initialized();

    if (s_logo_operation_mutex == nullptr) {
        ESP_LOGE(
            TAG,
            "Logo operation mutex is not initialized"
        );

        return false;
    }

    TickType_t timeout_ticks;

    if (timeout_ms == UINT32_MAX) {
        timeout_ticks = portMAX_DELAY;
    } else {
        timeout_ticks = pdMS_TO_TICKS(timeout_ms);

        /*
         * Preserve a small nonzero timeout when requested.
         */
        if (timeout_ms > 0 && timeout_ticks == 0) {
            timeout_ticks = 1;
        }
    }

    return xSemaphoreTake(
               s_logo_operation_mutex,
               timeout_ticks
           ) == pdTRUE;
}

void clock_logo_operation_end()
{
    if (s_logo_operation_mutex != nullptr) {
        xSemaphoreGive(s_logo_operation_mutex);
    }
}

static bool parse_header(const uint8_t header[20],
                         uint16_t *width,
                         uint16_t *height,
                         uint8_t *pixel_format,
                         uint8_t *flags,
                         uint16_t *reserved,
                         uint32_t *payload_length,
                         uint32_t *payload_crc)
{
    if (memcmp(header, "LGO1", 4) != 0) {
        return false;
    }

    uint16_t file_width = read_le_u16(&header[4]);
    uint16_t file_height = read_le_u16(&header[6]);
    uint8_t file_format = header[8];
    uint8_t file_flags = header[9];
    uint16_t file_reserved = read_le_u16(&header[10]);
    uint32_t file_payload_length = read_le_u32(&header[12]);
    uint32_t file_payload_crc = read_le_u32(&header[16]);

    if (width) {
        *width = file_width;
    }
    if (height) {
        *height = file_height;
    }
    if (pixel_format) {
        *pixel_format = file_format;
    }
    if (flags) {
        *flags = file_flags;
    }
    if (reserved) {
        *reserved = file_reserved;
    }
    if (payload_length) {
        *payload_length = file_payload_length;
    }
    if (payload_crc) {
        *payload_crc = file_payload_crc;
    }

    return file_width == CLOCK_LOGO_WIDTH &&
           file_height == CLOCK_LOGO_HEIGHT &&
           file_format == 1 &&
           file_flags == 0 &&
           file_reserved == 0 &&
           file_payload_length == CLOCK_LOGO_PAYLOAD_BYTES;
}

uint32_t clock_logo_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    if (data == nullptr) {
        return crc;
    }

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];

        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

uint32_t clock_logo_crc32_finalize(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

const uint32_t *clock_logo_lock_pixels()
{
    ensure_initialized();

    if (s_logo_mutex == nullptr) {
        return nullptr;
    }

    if (xSemaphoreTake(s_logo_mutex, portMAX_DELAY) != pdTRUE) {
        return nullptr;
    }

    return s_active_pixels;
}

void clock_logo_unlock_pixels()
{
    if (s_logo_mutex != nullptr) {
        xSemaphoreGive(s_logo_mutex);
    }
}

esp_err_t clock_logo_reload_from_sd()
{
    ensure_initialized();

    if (!clock_sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(kLogoPath, "rb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "Logo file missing: %s", kLogoPath);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[20];
    size_t header_read = fread(header, 1, sizeof(header), fp);
    if (header_read != sizeof(header)) {
        fclose(fp);
        ESP_LOGW(TAG, "Short logo header");
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t expected_crc = 0;

    if (!parse_header(header,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      &expected_crc)) {
        fclose(fp);
        ESP_LOGW(TAG, "Invalid logo header");
        return ESP_ERR_INVALID_ARG;
    }

    int target = 0;
    if (s_logo_mutex != nullptr && xSemaphoreTake(s_logo_mutex, portMAX_DELAY) == pdTRUE) {
        target = (s_active_pixels == s_logo_buffers[0]) ? 1 : 0;
        xSemaphoreGive(s_logo_mutex);
    }

    uint32_t *dest = s_logo_buffers[target];
    uint8_t chunk[384];
    uint32_t crc = 0xFFFFFFFFu;
    size_t pixels_written = 0;

    while (pixels_written < CLOCK_LOGO_PIXEL_COUNT) {
        size_t chunk_read = fread(chunk, 1, sizeof(chunk), fp);
        if (chunk_read != sizeof(chunk)) {
            fclose(fp);
            ESP_LOGW(TAG, "Short logo payload");
            return ESP_ERR_INVALID_SIZE;
        }

        crc = clock_logo_crc32_update(crc, chunk, chunk_read);

        for (size_t offset = 0; offset < chunk_read; offset += 3) {
            uint8_t r = chunk[offset];
            uint8_t g = chunk[offset + 1];
            uint8_t b = chunk[offset + 2];
            dest[pixels_written++] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    if (fgetc(fp) != EOF) {
        fclose(fp);
        ESP_LOGW(TAG, "Extra bytes in logo file");
        return ESP_ERR_INVALID_SIZE;
    }

    fclose(fp);

    uint32_t final_crc = clock_logo_crc32_finalize(crc);
    if (final_crc != expected_crc) {
        ESP_LOGW(TAG, "Logo CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    if (s_logo_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_logo_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    s_active_pixels = dest;
    xSemaphoreGive(s_logo_mutex);

    ESP_LOGI(TAG, "SD logo activated");
    return ESP_OK;
}

clock_logo_restore_result_t
clock_logo_restore_compiled_default()
{
    ensure_initialized();

    ESP_LOGI(TAG, "Restoring compiled default logo");

    /*
     * Do not wait if an upload or another logo mutation is active.
     */
    if (!clock_logo_operation_begin(0)) {
        ESP_LOGW(TAG, "Default-logo restore busy");
        return CLOCK_LOGO_RESTORE_BUSY;
    }

    clock_logo_restore_result_t result =
        CLOCK_LOGO_RESTORE_STORAGE_ERROR;

    /*
     * Successful restoration must guarantee that the uploaded logo
     * will not return after reboot.
     */
    if (!clock_sd_card_is_mounted()) {
        ESP_LOGE(
            TAG,
            "Default-logo restore failed: SD card is not mounted"
        );

        goto cleanup;
    }

    /*
     * Remove the permanent uploaded logo.
     * ENOENT means the desired persistent state already exists.
     */
    errno = 0;

    if (remove(kLogoPath) == 0) {
        ESP_LOGI(TAG, "SD logo file removed");
    } else if (errno == ENOENT) {
        ESP_LOGI(TAG, "SD logo file already absent");
    } else {
        ESP_LOGE(
            TAG,
            "Default-logo restore failed removing %s: errno=%d",
            kLogoPath,
            errno
        );

        goto cleanup;
    }

    /*
     * Remove an abandoned temporary upload.
     * logo.tmp is never loaded at boot, so failure is nonfatal.
     */
    errno = 0;

    if (remove(kTemporaryLogoPath) == 0) {
        ESP_LOGI(TAG, "Temporary logo file removed");
    } else if (errno != ENOENT) {
        ESP_LOGW(
            TAG,
            "Could not remove temporary logo file: errno=%d",
            errno
        );
    }

    /*
     * Remove an abandoned backup file used by the upload rollback logic.
     * logo.bak is not loaded at boot, so failure is also nonfatal.
     */
    errno = 0;

    if (remove(kBackupLogoPath) == 0) {
        ESP_LOGI(TAG, "Backup logo file removed");
    } else if (errno != ENOENT) {
        ESP_LOGW(
            TAG,
            "Could not remove backup logo file: errno=%d",
            errno
        );
    }

    /*
     * Safely switch the display to the compiled logo.
     */
    if (s_logo_mutex == nullptr) {
        ESP_LOGE(TAG, "Active-logo mutex is unavailable");
        goto cleanup;
    }

    if (xSemaphoreTake(
            s_logo_mutex,
            portMAX_DELAY
        ) != pdTRUE) {
        ESP_LOGE(TAG, "Could not lock active logo");
        goto cleanup;
    }

    s_active_pixels = logo_bitmap;

    xSemaphoreGive(s_logo_mutex);

    ESP_LOGI(TAG, "Compiled default logo activated");

    result = CLOCK_LOGO_RESTORE_OK;

cleanup:
    clock_logo_operation_end();

    if (result != CLOCK_LOGO_RESTORE_OK) {
        ESP_LOGE(TAG, "Default-logo restore failed");
    }

    return result;
}