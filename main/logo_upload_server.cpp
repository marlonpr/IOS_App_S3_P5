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
static bool s_server_desired = false;
static uint32_t s_server_generation = 0;
static int s_listen_sock = -1;
static int s_client_sock = -1;

static StaticSemaphore_t s_task_boot_gate_storage;
static SemaphoreHandle_t s_task_boot_gate = nullptr;
static StaticEventGroup_t s_start_event_group_storage;
static EventGroupHandle_t s_start_event_group = nullptr;

static bool logo_upload_server_mark_starting_if_idle(uint32_t *generation)
{
    portENTER_CRITICAL(&s_server_state_mux);
    bool idle = s_server_task_handle == nullptr && !s_server_starting;

    if (idle) {
        ++s_server_generation;
        s_server_desired = true;
        s_server_starting = true;
        s_server_listening = false;
        s_listen_sock = -1;
        s_client_sock = -1;
        if (generation != nullptr) {
            *generation = s_server_generation;
        }
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

static bool logo_upload_server_is_current(uint32_t generation)
{
    portENTER_CRITICAL(&s_server_state_mux);
    const bool current = s_server_desired &&
                         generation == s_server_generation;
    portEXIT_CRITICAL(&s_server_state_mux);
    return current;
}

static bool logo_upload_server_mark_listening(uint32_t generation,
                                              int listen_sock)
{
    portENTER_CRITICAL(&s_server_state_mux);
    const bool current = s_server_desired &&
                         generation == s_server_generation;
    if (current) {
        s_server_listening = true;
        s_listen_sock = listen_sock;
    }
    portEXIT_CRITICAL(&s_server_state_mux);
    return current;
}

static bool logo_upload_server_publish_client(uint32_t generation,
                                              int client_sock)
{
    portENTER_CRITICAL(&s_server_state_mux);
    const bool current = s_server_desired &&
                         generation == s_server_generation;
    if (current) {
        s_client_sock = client_sock;
    }
    portEXIT_CRITICAL(&s_server_state_mux);
    return current;
}

static bool logo_upload_server_release_client(uint32_t generation,
                                              int client_sock)
{
    bool owned = false;
    portENTER_CRITICAL(&s_server_state_mux);
    if (generation == s_server_generation && s_client_sock == client_sock) {
        s_client_sock = -1;
        owned = true;
    }
    portEXIT_CRITICAL(&s_server_state_mux);
    return owned;
}

static void logo_upload_server_clear_task_state(uint32_t generation)
{
    portENTER_CRITICAL(&s_server_state_mux);
    s_server_task_handle = nullptr;
    s_server_starting = false;
    s_server_listening = false;
    if (generation == s_server_generation) {
        s_server_desired = false;
        s_listen_sock = -1;
        s_client_sock = -1;
    }
    portEXIT_CRITICAL(&s_server_state_mux);
}

static void configure_tcp_keepalive(int socket_fd)
{
    int enable = 1;
    int idle_seconds = 60;
    int interval_seconds = 15;
    int probe_count = 4;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE,
                   &enable, sizeof(enable)) != 0 ||
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE,
                   &idle_seconds, sizeof(idle_seconds)) != 0 ||
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL,
                   &interval_seconds, sizeof(interval_seconds)) != 0 ||
        setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT,
                   &probe_count, sizeof(probe_count)) != 0) {
        ESP_LOGW(TAG, "TCP keepalive configuration failed: errno=%d", errno);
    }
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
    constexpr size_t kChunkSize = 384;

    struct timeval timeout = {};
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    uint8_t header[CLOCK_LOGO_HEADER_BYTES];
    bool timed_out = false;

    if (!recv_exact(client_sock, header, sizeof(header), &timed_out)) {
        send_error_line(client_sock, timed_out ? "ERR TIMEOUT\n" : "ERR SIZE\n");
        return;
    }

    clock_logo_file_info_t info = {};
    if (!clock_logo_parse_file_header(header, &info)) {
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

    while (err == ESP_OK && payload_received < info.payload_length) {
        size_t want = kChunkSize;
        if (want > (info.payload_length - payload_received)) {
            want = info.payload_length - payload_received;
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

    if (crc != info.payload_crc) {
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
    const uint32_t generation =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));

    if (s_task_boot_gate != nullptr) {
        xSemaphoreTake(s_task_boot_gate, portMAX_DELAY);
    }

    int listen_sock = -1;
    int client_sock = -1;
    int opt = 1;
    bool listen_published = false;

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

    if (!logo_upload_server_mark_listening(generation, listen_sock)) {
        close(listen_sock);
        listen_sock = -1;
        goto exit_task;
    }
    listen_published = true;
    ESP_LOGI(TAG, "Logo upload server listening on port %u", (unsigned)kUploadPort);

    if (s_start_event_group != nullptr) {
        xEventGroupSetBits(s_start_event_group, kServerReadyBit);
    }

	while (logo_upload_server_is_current(generation)) {
	    struct sockaddr_in client_addr = {};
	    socklen_t client_len = sizeof(client_addr);

	    client_sock = accept(
	        listen_sock,
	        reinterpret_cast<struct sockaddr *>(&client_addr),
	        &client_len
	    );

	    if (client_sock < 0) {
	        if (!logo_upload_server_is_current(generation)) {
	            break;
	        }
	        ESP_LOGE(TAG, "accept failed: errno=%d", errno);
	        continue;
	    }

	    if (!logo_upload_server_publish_client(generation, client_sock)) {
	        shutdown(client_sock, SHUT_RDWR);
	        close(client_sock);
	        client_sock = -1;
	        break;
	    }

	    configure_tcp_keepalive(client_sock);

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

	    if (logo_upload_server_release_client(generation, client_sock)) {
	        shutdown(client_sock, SHUT_RDWR);
	        close(client_sock);
	    }
	    client_sock = -1;
	}

exit_task:
    if (client_sock >= 0 &&
        logo_upload_server_release_client(generation, client_sock)) {
        shutdown(client_sock, SHUT_RDWR);
        close(client_sock);
    }

    if (listen_sock >= 0 &&
        (logo_upload_server_is_current(generation) || !listen_published)) {
        shutdown(listen_sock, SHUT_RDWR);
        close(listen_sock);
    }

    if (s_start_event_group != nullptr) {
        xEventGroupSetBits(s_start_event_group, kServerFailedBit);
    }

    /*
     * Publish completion before clearing the task handle.  A new generation
     * cannot start until the handle is clear, so its event-bit reset cannot
     * race this old generation's final failure notification.
     */
    logo_upload_server_clear_task_state(generation);

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
    uint32_t generation = 0;
    if (!logo_upload_server_mark_starting_if_idle(&generation)) {
        portENTER_CRITICAL(&s_server_state_mux);
        const bool current_listener = s_server_desired;
        portEXIT_CRITICAL(&s_server_state_mux);

        if (current_listener) {
            ESP_LOGI(TAG, "Logo upload server already running");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Previous logo server generation is still stopping");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task_boot_gate == nullptr) {
        s_task_boot_gate = xSemaphoreCreateBinaryStatic(&s_task_boot_gate_storage);
    }

    if (s_start_event_group == nullptr) {
        s_start_event_group = xEventGroupCreateStatic(&s_start_event_group_storage);
    }

    if (s_task_boot_gate == nullptr || s_start_event_group == nullptr) {
        ESP_LOGE(TAG, "Failed to start logo upload server: synchronization init failed");
        logo_upload_server_clear_task_state(generation);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_task_boot_gate, 0);
    xEventGroupClearBits(s_start_event_group, kServerReadyBit | kServerFailedBit);

    TaskHandle_t task_handle = nullptr;

    BaseType_t ret = xTaskCreatePinnedToCore(
        logo_upload_server_task,
        "LogoUploadServer",
        4096,
        reinterpret_cast<void *>(static_cast<uintptr_t>(generation)),
        4,
        &task_handle,
        0
    );

    if (ret != pdPASS) {
        logo_upload_server_clear_task_state(generation);
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

esp_err_t logo_upload_server_stop(void)
{
    int listen_sock = -1;
    int client_sock = -1;

    portENTER_CRITICAL(&s_server_state_mux);
    if (s_server_task_handle == nullptr && !s_server_starting) {
        s_server_desired = false;
        s_server_listening = false;
        portEXIT_CRITICAL(&s_server_state_mux);
        return ESP_OK;
    }

    s_server_desired = false;
    s_server_listening = false;
    ++s_server_generation;
    listen_sock = s_listen_sock;
    client_sock = s_client_sock;
    s_listen_sock = -1;
    s_client_sock = -1;
    portEXIT_CRITICAL(&s_server_state_mux);

    if (client_sock >= 0) {
        shutdown(client_sock, SHUT_RDWR);
        close(client_sock);
    }
    if (listen_sock >= 0) {
        shutdown(listen_sock, SHUT_RDWR);
        close(listen_sock);
    }

    for (uint32_t waited_ms = 0; waited_ms < 1000; waited_ms += 10) {
        portENTER_CRITICAL(&s_server_state_mux);
        const bool running = s_server_task_handle != nullptr ||
                             s_server_starting;
        portEXIT_CRITICAL(&s_server_state_mux);

        if (!running) {
            ESP_LOGI(TAG, "Logo upload listener and client invalidated");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Timed out waiting for logo upload server to stop");
    return ESP_ERR_TIMEOUT;
}
