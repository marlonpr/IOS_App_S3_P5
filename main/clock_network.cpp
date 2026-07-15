#include "clock_network.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "freertos/portmacro.h"

#include "esp_log.h"

#include "clock_mdns.h"
#include "clock_ota.h"
#include "clock_wifi.h"
#include "logo_upload_server.h"

namespace {

constexpr uint8_t kBoardId = 0;
constexpr uint32_t kResultMessageDurationMs = 3000;
constexpr uint32_t kRouteWaitMs = 3000;
constexpr uint32_t kRoutePollMs = 100;
const char *const TAG = "CLOCK_NETWORK";

portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

int s_boot_mode = CLOCK_NETWORK_MODE_AUTO;
int s_active_interface = CLOCK_NETWORK_INTERFACE_NONE;
bool s_task_started = false;

clock_ethernet_rx_callback_t s_rx_callback = nullptr;
clock_network_show_message_callback_t s_show_message = nullptr;
clock_network_service_guard_t s_service_guard = {};

bool claim_network_task_start(void)
{
    bool should_start = false;

    portENTER_CRITICAL(&s_state_lock);

    if (!s_task_started) {
        s_task_started = true;
        should_start = true;
    }

    portEXIT_CRITICAL(&s_state_lock);

    return should_start;
}

void clear_network_task_started(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_task_started = false;
    portEXIT_CRITICAL(&s_state_lock);
}

void set_boot_mode(clock_network_mode_t mode)
{
    portENTER_CRITICAL(&s_state_lock);
    s_boot_mode = static_cast<int>(mode);
    portEXIT_CRITICAL(&s_state_lock);
}

clock_network_mode_t get_boot_mode_locked(void)
{
    int mode;

    portENTER_CRITICAL(&s_state_lock);
    mode = s_boot_mode;
    portEXIT_CRITICAL(&s_state_lock);

    return static_cast<clock_network_mode_t>(mode);
}

void set_active_interface(clock_network_interface_t interface)
{
    portENTER_CRITICAL(&s_state_lock);
    s_active_interface = static_cast<int>(interface);
    portEXIT_CRITICAL(&s_state_lock);
}

clock_network_interface_t get_active_interface_locked(void)
{
    int interface;

    portENTER_CRITICAL(&s_state_lock);
    interface = s_active_interface;
    portEXIT_CRITICAL(&s_state_lock);

    return static_cast<clock_network_interface_t>(interface);
}

bool active_route_is_ready(clock_network_interface_t interface)
{
    switch (interface) {
        case CLOCK_NETWORK_INTERFACE_ETHERNET:
            return clock_ethernet_route_is_ready();
        case CLOCK_NETWORK_INTERFACE_WIFI:
            return clock_wifi_route_is_ready();
        case CLOCK_NETWORK_INTERFACE_NONE:
        default:
            return false;
    }
}

bool wait_for_active_route(clock_network_interface_t interface)
{
    for (uint32_t waited_ms = 0; waited_ms <= kRouteWaitMs;
         waited_ms += kRoutePollMs) {
        if (active_route_is_ready(interface)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kRoutePollMs));
    }

    return false;
}

void log_service_error(const char *service, esp_err_t result)
{
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "%s startup failed: %s",
                 service,
                 esp_err_to_name(result));
    }
}

void start_network_services_once(clock_network_interface_t interface)
{
    if (!clock_network_service_guard_try_start(&s_service_guard)) {
        ESP_LOGW(TAG, "Network services already started; duplicate ignored");
        return;
    }

    ESP_LOGI(TAG,
             "Starting shared network services once on %s",
             interface == CLOCK_NETWORK_INTERFACE_ETHERNET ? "Ethernet" : "Wi-Fi");

    log_service_error("TCP server",
                      clock_ethernet_start_tcp_server(s_rx_callback));
    log_service_error("mDNS", clock_mdns_start(kBoardId));

    const esp_err_t upload_result = logo_upload_server_start();
    log_service_error("Logo upload server", upload_result);
    if (upload_result == ESP_OK) {
        log_service_error("Logo upload mDNS",
                          logo_upload_server_register_mdns_service(kBoardId));
    }

    if (wait_for_active_route(interface)) {
        log_service_error("Startup OTA check",
                          clock_ota_start_github_check_once());
    } else {
        ESP_LOGW(TAG,
                 "IP is assigned but gateway/DNS are not ready; startup OTA check not launched");
    }
}

bool start_and_wait_ethernet(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "Starting W5500 Ethernet boot attempt");
    const esp_err_t result = clock_ethernet_init_dhcp();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "W5500 initialization failed: %s",
                 esp_err_to_name(result));
        return false;
    }

    return clock_ethernet_wait_for_ip(timeout_ms);
}

bool start_and_wait_wifi(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "Starting Wi-Fi STA boot attempt");
    const esp_err_t result = clock_wifi_init_sta();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s",
                 esp_err_to_name(result));
        return false;
    }

    return clock_wifi_wait_for_ip(timeout_ms);
}

void show_result(clock_network_interface_t interface)
{
    const char *message = clock_network_result_message(interface);
    ESP_LOGI(TAG, "%s", message);
    if (s_show_message != nullptr) {
        s_show_message(message, kResultMessageDurationMs);
    }
}

void network_boot_task(void *)
{
	const clock_network_mode_t mode = get_boot_mode_locked();
    clock_network_interface_t active = CLOCK_NETWORK_INTERFACE_NONE;

    ESP_LOGI(TAG, "Saved boot network mode: %s",
             clock_network_mode_label(mode));

    switch (mode) {
        case CLOCK_NETWORK_MODE_AUTO:
            if (start_and_wait_ethernet(CONFIG_CLOCK_NETWORK_AUTO_ETH_TIMEOUT_MS)) {
                active = CLOCK_NETWORK_INTERFACE_ETHERNET;
                break;
            }

            ESP_LOGW(TAG, "Ethernet failed in Auto mode; trying Wi-Fi");
            (void)clock_ethernet_stop();
            if (s_show_message != nullptr) {
                s_show_message("TRY WIFI", 1500);
            }

            if (start_and_wait_wifi(CONFIG_CLOCK_NETWORK_AUTO_WIFI_TIMEOUT_MS)) {
                active = CLOCK_NETWORK_INTERFACE_WIFI;
            } else {
                (void)clock_wifi_stop();
            }
            break;

        case CLOCK_NETWORK_MODE_ETHERNET:
            if (start_and_wait_ethernet(CONFIG_CLOCK_NETWORK_ETH_TIMEOUT_MS)) {
                active = CLOCK_NETWORK_INTERFACE_ETHERNET;
            }
            break;

        case CLOCK_NETWORK_MODE_WIFI:
            if (start_and_wait_wifi(CONFIG_CLOCK_NETWORK_WIFI_TIMEOUT_MS)) {
                active = CLOCK_NETWORK_INTERFACE_WIFI;
            } else {
                (void)clock_wifi_stop();
            }
            break;
    }

    set_active_interface(active);

    if (active != CLOCK_NETWORK_INTERFACE_NONE) {
        start_network_services_once(active);
    } else {
        ESP_LOGW(TAG, "No interface obtained an IP; network services remain stopped");
    }

    show_result(active);
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t clock_network_start(clock_network_mode_t boot_mode,
                              clock_ethernet_rx_callback_t rx_callback,
                              clock_network_show_message_callback_t show_message)
{
    if (!clock_network_mode_is_valid(static_cast<uint8_t>(boot_mode)) ||
        rx_callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

	if (!claim_network_task_start()) {
	    return ESP_ERR_INVALID_STATE;
	}

	set_boot_mode(boot_mode);
	set_active_interface(CLOCK_NETWORK_INTERFACE_NONE);
    s_rx_callback = rx_callback;
    s_show_message = show_message;
    s_service_guard.started = false;

    const BaseType_t result = xTaskCreatePinnedToCore(
        network_boot_task,
        "NetworkBootTask",
        6144,
        nullptr,
        2,
        nullptr,
        0);

    if (result != pdPASS) {
        clear_network_task_started();
        ESP_LOGE(TAG, "Failed to create network boot task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

clock_network_mode_t clock_network_get_boot_mode(void)
{
    return get_boot_mode_locked();
}

clock_network_interface_t clock_network_get_active_interface(void)
{
    return get_active_interface_locked();
}

bool clock_network_services_started(void)
{
    return s_service_guard.started;
}
