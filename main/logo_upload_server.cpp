#include "logo_upload_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "clock_logo_manager.h"
#include "clock_mdns.h"
#include "clock_modes.h"
#include "clock_sd_card.h"

static const char *TAG = "LOGO_UPLOAD";
static const uint16_t kUploadPort = 5001;
static const char *const kTempPath = "/sdcard/logo.tmp";
static const char *const kLogoPath = "/sdcard/logo.lgo";
static const char *const kBackupPath = "/sdcard/logo.bak";
static constexpr EventBits_t kServerReadyBit = BIT0;
static constexpr EventBits_t kServerFailedBit = BIT1;

static portMUX_TYPE s_server_state_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_server_task_handle = nullptr;
static bool s_server_starting = false;
static bool s_server_listening = false;
static bool s_service_registered = false;

static StaticSemaphore_t s_task_boot_gate_storage;
static SemaphoreHandle_t s_task_boot_gate = nullptr;
static StaticEventGroup_t s_start_event_group_storage;
static EventGroupHandle_t s_start_event_group = nullptr;

static bool logo_upload_server_mark_starting_if_idle()
{
    portENTER_CRITICAL(&s_server_state_mux);
    bool idle = s_server_task_handle == nullptr && !s_server_starting;

    if (idle) {
        s_server_starting = true;
        s_server_listening = false;
    }

    portEXIT_CRITICAL(&s_server_state_mux);

    return idle;
}

static bool logo_upload_server_is_listening()
{
    portENTER_CRITICAL(&s_server_state_mux);
    bool listening = s_server_listening;
    portEXIT_CRITICAL(&s_server_state_mux);

    return listening;
}

static void logo_upload_server_mark_listening()
{
    portENTER_CRITICAL(&s_server_state_mux);
    s_server_listening = true;
    portEXIT_CRITICAL(&s_server_state_mux);
}

static void logo_upload_server_clear_task_state()
{
    portENTER_CRITICAL(&s_server_state_mux);
    s_server_task_handle = nullptr;
    s_server_starting = false;
    s_server_listening = false;
    portEXIT_CRITICAL(&s_server_state_mux);
}

static bool send_all(int sock, const char *data)
{
    if (data == nullptr) {
        return false;
    }

    size_t len = strlen(data);
    size_t sent_total = 0;

    while (sent_total < len) {
        ssize_t sent = send(sock, data + sent_total, len - sent_total, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                return false;
            }

            return false;
        }

        if (sent == 0) {
            return false;
        }

        sent_total += (size_t)sent;
    }

    return true;
}

static bool recv_exact(int sock, uint8_t *buffer, size_t len, bool *timed_out)
{
    if (buffer == nullptr || len == 0) {
        return true;
    }

    size_t received = 0;

    while (received < len) {
        ssize_t ret = recv(sock, buffer + received, len - received, 0);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                if (timed_out) {
                    *timed_out = true;
                }
                return false;
            }

            return false;
        }

        if (ret == 0) {
            return false;
        }

        received += (size_t)ret;
    }

    return true;
}

static bool send_error_line(int sock, const char *line)
{
    return send_all(sock, line);
}

static bool parse_header(const uint8_t header[20], uint32_t *expected_crc)
{
    if (memcmp(header, "LGO1", 4) != 0) {
        return false;
    }

    uint16_t width = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
    uint16_t height = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
    uint8_t pixel_format = header[8];
    uint8_t flags = header[9];
    uint16_t reserved = (uint16_t)header[10] | ((uint16_t)header[11] << 8);
    uint32_t payload_length = (uint32_t)header[12] |
                              ((uint32_t)header[13] << 8) |
                              ((uint32_t)header[14] << 16) |
                              ((uint32_t)header[15] << 24);
    uint32_t crc = (uint32_t)header[16] |
                   ((uint32_t)header[17] << 8) |
                   ((uint32_t)header[18] << 16) |
                   ((uint32_t)header[19] << 24);

    if (width != CLOCK_LOGO_WIDTH ||
        height != CLOCK_LOGO_HEIGHT ||
        pixel_format != 1 ||
        flags != 0 ||
        reserved != 0 ||
        payload_length != CLOCK_LOGO_PAYLOAD_BYTES) {
        return false;
    }

    if (expected_crc != nullptr) {
        *expected_crc = crc;
    }

    return true;
}

static esp_err_t stage_logo_file()
{
    unlink(kBackupPath);

    if (rename(kLogoPath, kBackupPath) != 0) {
        if (errno != ENOENT) {
            return ESP_FAIL;
        }
    }

    if (rename(kTempPath, kLogoPath) != 0) {
        rename(kBackupPath, kLogoPath);
        unlink(kBackupPath);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void finalize_logo_file()
{
    unlink(kBackupPath);
}

static void rollback_logo_file()
{
    if (access(kBackupPath, F_OK) == 0) {
        rename(kBackupPath, kLogoPath);
        unlink(kBackupPath);
    }
}

static void handle_client(int client_sock)
{
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kPayloadSize = CLOCK_LOGO_PAYLOAD_BYTES;
    constexpr size_t kChunkSize = 384;

    struct timeval timeout = {};
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    uint8_t header[kHeaderSize];
    bool timed_out = false;

    if (!recv_exact(client_sock, header, sizeof(header), &timed_out)) {
        send_error_line(client_sock, timed_out ? "ERR TIMEOUT\n" : "ERR SIZE\n");
        return;
    }

    uint32_t expected_crc = 0;
    if (!parse_header(header, &expected_crc)) {
        send_error_line(client_sock, "ERR HEADER\n");
        return;
    }

    if (!send_all(client_sock, "READY\n")) {
        return;
    }

    if (!clock_sd_card_is_mounted()) {
        send_error_line(client_sock, "ERR SD\n");
        return;
    }

    FILE *fp = fopen(kTempPath, "wb");
    if (fp == nullptr) {
        send_error_line(client_sock, "ERR WRITE\n");
        return;
    }

    esp_err_t err = ESP_OK;
    uint8_t chunk[kChunkSize];
    size_t payload_received = 0;
    uint32_t crc = 0xFFFFFFFFu;

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        err = ESP_FAIL;
    }

    while (err == ESP_OK && payload_received < kPayloadSize) {
        size_t want = kChunkSize;
        if (want > (kPayloadSize - payload_received)) {
            want = kPayloadSize - payload_received;
        }

        timed_out = false;
        if (!recv_exact(client_sock, chunk, want, &timed_out)) {
            err = timed_out ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_SIZE;
            break;
        }

        crc = clock_logo_crc32_update(crc, chunk, want);

        if (fwrite(chunk, 1, want, fp) != want) {
            err = ESP_FAIL;
            break;
        }

        payload_received += want;
    }

    if (err == ESP_OK) {
        uint8_t extra_byte = 0;
        ssize_t extra = recv(client_sock, &extra_byte, 1, MSG_DONTWAIT);
        if (extra > 0) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    if (err == ESP_OK && fflush(fp) != 0) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK && fsync(fileno(fp)) != 0) {
        err = ESP_FAIL;
    }

    if (fclose(fp) != 0) {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
    }

    if (err != ESP_OK) {
        unlink(kTempPath);

        if (err == ESP_ERR_TIMEOUT) {
            send_error_line(client_sock, "ERR TIMEOUT\n");
        } else if (err == ESP_ERR_INVALID_SIZE) {
            send_error_line(client_sock, "ERR SIZE\n");
        } else {
            send_error_line(client_sock, "ERR WRITE\n");
        }
        return;
    }

    crc = clock_logo_crc32_finalize(crc);

    if (crc != expected_crc) {
        unlink(kTempPath);
        send_error_line(client_sock, "ERR CRC\n");
        return;
    }

    err = stage_logo_file();
    if (err != ESP_OK) {
        unlink(kTempPath);
        send_error_line(client_sock, "ERR SD\n");
        return;
    }

    err = clock_logo_reload_from_sd();
    if (err != ESP_OK) {
        rollback_logo_file();
        send_error_line(client_sock, "ERR LOAD\n");
        return;
    }

    finalize_logo_file();
    clock_modes_reset_sequences();

    send_all(client_sock, "OK\n");
}

static void logo_upload_server_task(void *param)
{
    (void)param;

    if (s_task_boot_gate != nullptr) {
        xSemaphoreTake(s_task_boot_gate, portMAX_DELAY);
    }

    int listen_sock = -1;
    int client_sock = -1;
    int opt = 1;

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(kUploadPort);

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        goto exit_task;
    }

    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        goto exit_task;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        goto exit_task;
    }

    logo_upload_server_mark_listening();
    ESP_LOGI(TAG, "Logo upload server listening on port %u", (unsigned)kUploadPort);

    if (s_start_event_group != nullptr) {
        xEventGroupSetBits(s_start_event_group, kServerReadyBit);
    }

	while (true) {
	    struct sockaddr_in client_addr = {};
	    socklen_t client_len = sizeof(client_addr);

	    client_sock = accept(
	        listen_sock,
	        reinterpret_cast<struct sockaddr *>(&client_addr),
	        &client_len
	    );

	    if (client_sock < 0) {
	        ESP_LOGE(TAG, "accept failed: errno=%d", errno);
	        continue;
	    }

	    ESP_LOGI(
	        TAG,
	        "Upload client connected from %s:%u",
	        inet_ntoa(client_addr.sin_addr),
	        static_cast<unsigned>(ntohs(client_addr.sin_port))
	    );

	    if (!clock_logo_operation_begin(10000)) {
	        ESP_LOGW(
	            TAG,
	            "Logo upload could not acquire operation lock"
	        );

	        send_error_line(client_sock, "ERR TIMEOUT\n");
	    } else {
	        handle_client(client_sock);
	        clock_logo_operation_end();
	    }

	    shutdown(client_sock, 0);
	    close(client_sock);
	    client_sock = -1;
	}

exit_task:
    if (client_sock >= 0) {
        shutdown(client_sock, 0);
        close(client_sock);
    }

    if (listen_sock >= 0) {
        close(listen_sock);
    }

    logo_upload_server_clear_task_state();

    if (s_start_event_group != nullptr) {
        xEventGroupSetBits(s_start_event_group, kServerFailedBit);
    }

    vTaskDelete(NULL);
}

esp_err_t logo_upload_server_register_mdns_service(uint8_t board_id)
{
    if (!logo_upload_server_is_listening()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_service_registered) {
        return ESP_OK;
    }

    esp_err_t err = clock_mdns_add_logo_upload_service(board_id);
    if (err != ESP_OK) {
        return err;
    }

    s_service_registered = true;
    return ESP_OK;
}

esp_err_t logo_upload_server_start(void)
{
    if (!logo_upload_server_mark_starting_if_idle()) {
        ESP_LOGI(TAG, "Logo upload server already running");
        return ESP_OK;
    }

    if (s_task_boot_gate == nullptr) {
        s_task_boot_gate = xSemaphoreCreateBinaryStatic(&s_task_boot_gate_storage);
    }

    if (s_start_event_group == nullptr) {
        s_start_event_group = xEventGroupCreateStatic(&s_start_event_group_storage);
    }

    if (s_task_boot_gate == nullptr || s_start_event_group == nullptr) {
        ESP_LOGE(TAG, "Failed to start logo upload server: synchronization init failed");
        logo_upload_server_clear_task_state();
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_task_boot_gate, 0);
    xEventGroupClearBits(s_start_event_group, kServerReadyBit | kServerFailedBit);

    TaskHandle_t task_handle = nullptr;

    BaseType_t ret = xTaskCreatePinnedToCore(
        logo_upload_server_task,
        "LogoUploadServer",
        4096,
        NULL,
        4,
        &task_handle,
        0
    );

    if (ret != pdPASS) {
        logo_upload_server_clear_task_state();
        ESP_LOGE(TAG, "Failed to start logo upload server: task creation failed");
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_server_state_mux);
    s_server_task_handle = task_handle;
    s_server_starting = false;
    s_server_listening = false;
    portEXIT_CRITICAL(&s_server_state_mux);

    xSemaphoreGive(s_task_boot_gate);

    EventBits_t bits = xEventGroupWaitBits(
        s_start_event_group,
        kServerReadyBit | kServerFailedBit,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(5000)
    );

    if ((bits & kServerReadyBit) != 0) {
        return ESP_OK;
    }

    if ((bits & kServerFailedBit) != 0) {
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Failed to start logo upload server: startup timed out");
    return ESP_ERR_TIMEOUT;
}
