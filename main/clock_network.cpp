#include "clock_network.h"

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"

#include "clock_mdns.h"
#include "clock_ota.h"
#include "clock_wifi.h"
#include "logo_upload_server.h"

namespace {

constexpr uint8_t kBoardId = 0;
constexpr uint32_t kResultMessageDurationMs = 3000;
constexpr uint32_t kRouteWaitMs = 3000;
constexpr uint32_t kRoutePollMs = 100;
const char *const TAG = "NET";

enum : uint32_t {
    kPendingGotIp = BIT0,
    kPendingNetworkLoss = BIT1,
};

struct pending_network_events_t {
    uint32_t bits;
    bool loss_was_online;
};

portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

int s_boot_mode = CLOCK_NETWORK_MODE_AUTO;
int s_active_interface = CLOCK_NETWORK_INTERFACE_NONE;
int s_network_status = CLOCK_NETWORK_CONNECTING;
uint8_t s_retry_attempt = 0;
bool s_task_started = false;
bool s_services_started = false;
bool s_shared_services_started_once = false;
uint32_t s_pending_events = 0;
bool s_pending_loss_was_online = false;
TaskHandle_t s_supervisor_task = nullptr;

clock_ethernet_rx_callback_t s_rx_callback = nullptr;
clock_network_show_message_callback_t s_show_message = nullptr;

const char *status_label(clock_network_status_t status)
{
    switch (status) {
        case CLOCK_NETWORK_CONNECTING:
            return "CONNECTING";
        case CLOCK_NETWORK_ONLINE:
            return "ONLINE";
        case CLOCK_NETWORK_OFFLINE:
        default:
            return "OFFLINE";
    }
}

uint64_t now_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000LL);
}

bool claim_network_task_start()
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

void clear_network_task_started()
{
    portENTER_CRITICAL(&s_state_lock);
    s_task_started = false;
    s_supervisor_task = nullptr;
    portEXIT_CRITICAL(&s_state_lock);
}

void set_boot_mode(clock_network_mode_t mode)
{
    portENTER_CRITICAL(&s_state_lock);
    s_boot_mode = static_cast<int>(mode);
    portEXIT_CRITICAL(&s_state_lock);
}

clock_network_mode_t get_boot_mode_locked()
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

clock_network_interface_t get_active_interface_locked()
{
    int interface;
    portENTER_CRITICAL(&s_state_lock);
    interface = s_active_interface;
    portEXIT_CRITICAL(&s_state_lock);
    return static_cast<clock_network_interface_t>(interface);
}

clock_network_status_t set_network_status(clock_network_status_t status)
{
    int old_status;
    portENTER_CRITICAL(&s_state_lock);
    old_status = s_network_status;
    s_network_status = static_cast<int>(status);
    portEXIT_CRITICAL(&s_state_lock);

    const auto old = static_cast<clock_network_status_t>(old_status);
    if (old != status) {
        ESP_LOGI(TAG, "status %s -> %s", status_label(old), status_label(status));
    }
    return old;
}

void set_retry_attempt(uint8_t attempt)
{
    portENTER_CRITICAL(&s_state_lock);
    s_retry_attempt = attempt;
    portEXIT_CRITICAL(&s_state_lock);
}

uint8_t get_retry_attempt()
{
    portENTER_CRITICAL(&s_state_lock);
    const uint8_t attempt = s_retry_attempt;
    portEXIT_CRITICAL(&s_state_lock);
    return attempt;
}

void set_services_started(bool started)
{
    portENTER_CRITICAL(&s_state_lock);
    s_services_started = started;
    portEXIT_CRITICAL(&s_state_lock);
}

pending_network_events_t take_pending_events()
{
    pending_network_events_t events = {};
    portENTER_CRITICAL(&s_state_lock);
    events.bits = s_pending_events;
    events.loss_was_online = s_pending_loss_was_online;
    s_pending_events = 0;
    s_pending_loss_was_online = false;
    portEXIT_CRITICAL(&s_state_lock);
    return events;
}

void wake_supervisor()
{
    TaskHandle_t task = nullptr;
    portENTER_CRITICAL(&s_state_lock);
    task = s_supervisor_task;
    portEXIT_CRITICAL(&s_state_lock);

    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

void network_interface_event(clock_network_interface_event_t event,
                             uint32_t detail,
                             void *context)
{
    const auto interface = static_cast<clock_network_interface_t>(
        reinterpret_cast<uintptr_t>(context));
    if (interface != get_active_interface_locked()) {
        return;
    }

    if (event == CLOCK_NETWORK_INTERFACE_EVENT_STARTED ||
        event == CLOCK_NETWORK_INTERFACE_EVENT_LINK_UP) {
        return;
    }

    if (event == CLOCK_NETWORK_INTERFACE_EVENT_GOT_IP) {
        if (detail == 0) {
            return;
        }

        set_network_status(CLOCK_NETWORK_ONLINE);
        set_retry_attempt(0);
        portENTER_CRITICAL(&s_state_lock);
        s_pending_events |= kPendingGotIp;
        portEXIT_CRITICAL(&s_state_lock);

        const esp_ip4_addr_t address = {.addr = detail};
        ESP_LOGI(TAG, "%s local IP acquired: " IPSTR,
                 interface == CLOCK_NETWORK_INTERFACE_WIFI ? "Wi-Fi" : "Ethernet",
                 IP2STR(&address));
        wake_supervisor();
        return;
    }

    if (event != CLOCK_NETWORK_INTERFACE_EVENT_DISCONNECTED &&
        event != CLOCK_NETWORK_INTERFACE_EVENT_LOST_IP &&
        event != CLOCK_NETWORK_INTERFACE_EVENT_STOPPED) {
        return;
    }

    const clock_network_status_t status = clock_network_get_status();
    const bool loss_was_online = status == CLOCK_NETWORK_ONLINE;
    if (loss_was_online) {
        set_network_status(CLOCK_NETWORK_CONNECTING);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_pending_events |= kPendingNetworkLoss;
    s_pending_loss_was_online =
        s_pending_loss_was_online || loss_was_online;
    portEXIT_CRITICAL(&s_state_lock);

    if (interface == CLOCK_NETWORK_INTERFACE_WIFI &&
        event == CLOCK_NETWORK_INTERFACE_EVENT_DISCONNECTED) {
        uint8_t attempt = get_retry_attempt();
        if (attempt == 0) {
            attempt = 1;
        }
        ESP_LOGW(TAG,
                 "Wi-Fi disconnected reason=%" PRIu32
                 " mode=%s attempt=%u/%u phase=%s",
                 detail,
                 clock_network_mode_label(get_boot_mode_locked()),
                 static_cast<unsigned>(attempt),
                 static_cast<unsigned>(CLOCK_NETWORK_ATTEMPTS_PER_BURST),
                 status == CLOCK_NETWORK_OFFLINE
                     ? "offline-cooldown"
                     : "reconnect-burst");
    }

    wake_supervisor();
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
        ESP_LOGE(TAG, "%s operation failed: %s",
                 service,
                 esp_err_to_name(result));
    }
}

void stop_network_services()
{
    log_service_error("TCP server stop", clock_ethernet_stop_tcp_server());
    log_service_error("Logo upload server stop", logo_upload_server_stop());
    set_services_started(false);
}

void start_network_services(clock_network_interface_t interface,
                            bool recovery)
{
    const esp_err_t tcp_result =
        clock_ethernet_start_tcp_server(s_rx_callback);
    log_service_error("TCP server", tcp_result);

    const esp_err_t upload_result = logo_upload_server_start();
    log_service_error("Logo upload server", upload_result);

    log_service_error("mDNS", clock_mdns_start(kBoardId));
    if (upload_result == ESP_OK) {
        log_service_error("Logo upload mDNS",
                          logo_upload_server_register_mdns_service(kBoardId));
    }

    const bool listeners_started =
        tcp_result == ESP_OK && upload_result == ESP_OK;
    set_services_started(listeners_started);

    if (!s_shared_services_started_once) {
        s_shared_services_started_once = true;
        if (wait_for_active_route(interface)) {
            log_service_error("Startup OTA check",
                              clock_ota_start_github_check_once());
        } else {
            ESP_LOGW(TAG,
                     "Local IP is online but gateway/DNS are unavailable; OTA check skipped");
        }
    }

    if (recovery && listeners_started) {
        ESP_LOGI(TAG, "TCP services restarted after network recovery");
    }
}

uint32_t connection_timeout_ms(clock_network_mode_t mode,
                               clock_network_interface_t interface)
{
    if (interface == CLOCK_NETWORK_INTERFACE_ETHERNET) {
        return mode == CLOCK_NETWORK_MODE_AUTO
                   ? CONFIG_CLOCK_NETWORK_AUTO_ETH_TIMEOUT_MS
                   : CONFIG_CLOCK_NETWORK_ETH_TIMEOUT_MS;
    }

    return mode == CLOCK_NETWORK_MODE_AUTO
               ? CONFIG_CLOCK_NETWORK_AUTO_WIFI_TIMEOUT_MS
               : CONFIG_CLOCK_NETWORK_WIFI_TIMEOUT_MS;
}

uint32_t connection_timeout_within_slot(
    clock_network_mode_t mode,
    clock_network_interface_t interface,
    uint64_t slot_deadline_ms)
{
    const uint64_t current_ms = now_ms();
    if (current_ms >= slot_deadline_ms) {
        return 0;
    }

    const uint64_t remaining_ms = slot_deadline_ms - current_ms;
    const uint32_t configured_ms = connection_timeout_ms(mode, interface);
    return remaining_ms < configured_ms
               ? static_cast<uint32_t>(remaining_ms)
               : configured_ms;
}

bool attempt_interface(clock_network_interface_t interface,
                       uint32_t timeout_ms,
                       bool stop_on_failure)
{
    if (timeout_ms == 0) {
        return false;
    }

    set_active_interface(interface);
    set_network_status(CLOCK_NETWORK_CONNECTING);

    esp_err_t result = ESP_FAIL;
    bool connected = false;

    if (interface == CLOCK_NETWORK_INTERFACE_ETHERNET) {
        ESP_LOGI(TAG, "Starting W5500 Ethernet connection attempt");
        result = clock_ethernet_init_dhcp();
        if (result == ESP_OK) {
            connected = clock_ethernet_wait_for_ip(timeout_ms);
        }
    } else {
        ESP_LOGI(TAG, "Starting Wi-Fi STA connection attempt");
        result = clock_wifi_init_sta();
        if (result == ESP_OK) {
            connected = clock_wifi_wait_for_ip(timeout_ms);
        }
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "%s initialization/start failed: %s",
                 interface == CLOCK_NETWORK_INTERFACE_ETHERNET
                     ? "Ethernet"
                     : "Wi-Fi",
                 esp_err_to_name(result));
    }

    if (connected || clock_network_get_status() == CLOCK_NETWORK_ONLINE) {
        return true;
    }

    if (result != ESP_OK || stop_on_failure) {
        if (interface == CLOCK_NETWORK_INTERFACE_ETHERNET) {
            (void)clock_ethernet_stop();
        } else {
            (void)clock_wifi_stop();
        }
    }
    return false;
}

bool perform_connection_attempt(clock_network_mode_t mode,
                                uint64_t slot_deadline_ms)
{
    const clock_network_interface_t active = get_active_interface_locked();
    if (active != CLOCK_NETWORK_INTERFACE_NONE) {
        return attempt_interface(
            active,
            connection_timeout_within_slot(mode, active, slot_deadline_ms),
            false);
    }

    const clock_network_interface_t first = clock_network_first_interface(mode);
    const clock_network_interface_t fallback =
        clock_network_fallback_interface(mode, first);
    if (attempt_interface(
            first,
            connection_timeout_within_slot(mode, first, slot_deadline_ms),
            fallback != CLOCK_NETWORK_INTERFACE_NONE)) {
        return true;
    }

    if (fallback == CLOCK_NETWORK_INTERFACE_NONE) {
        set_active_interface(first);
        return false;
    }

    ESP_LOGW(TAG, "Ethernet failed in Auto mode; trying Wi-Fi");
    if (s_show_message != nullptr) {
        s_show_message("TRY WIFI", 1500);
    }

    if (attempt_interface(
            fallback,
            connection_timeout_within_slot(mode, fallback, slot_deadline_ms),
            true)) {
        return true;
    }

    set_active_interface(CLOCK_NETWORK_INTERFACE_NONE);
    return false;
}

const char *reconnect_target_label(clock_network_mode_t mode)
{
    switch (get_active_interface_locked()) {
        case CLOCK_NETWORK_INTERFACE_ETHERNET:
            return "Ethernet";
        case CLOCK_NETWORK_INTERFACE_WIFI:
            return "Wi-Fi";
        case CLOCK_NETWORK_INTERFACE_NONE:
        default:
            return clock_network_mode_label(mode);
    }
}

void stop_selected_interface()
{
    switch (get_active_interface_locked()) {
        case CLOCK_NETWORK_INTERFACE_ETHERNET:
            (void)clock_ethernet_stop();
            break;
        case CLOCK_NETWORK_INTERFACE_WIFI:
            (void)clock_wifi_stop();
            break;
        case CLOCK_NETWORK_INTERFACE_NONE:
        default:
            break;
    }
}

void show_result(clock_network_interface_t interface)
{
    const char *message = clock_network_result_message(interface);
    ESP_LOGI(TAG, "%s", message);
    if (s_show_message != nullptr) {
        s_show_message(message, kResultMessageDurationMs);
    }
}

void network_supervisor_task(void *)
{
    const clock_network_mode_t mode = get_boot_mode_locked();
    clock_network_retry_state_t retry = {};
    clock_network_retry_state_init(&retry, now_ms());
    bool startup_result_shown = false;
    bool services_were_recovered = false;

    ESP_LOGI(TAG, "Persistent supervisor started; saved mode=%s",
             clock_network_mode_label(mode));
    ESP_LOGI(TAG, "Starting initial reconnect burst");

    for (;;) {
        const pending_network_events_t pending = take_pending_events();
        (void)pending.bits;
        if (pending.loss_was_online) {
            stop_network_services();
            clock_network_retry_disconnected(&retry, now_ms());
            set_retry_attempt(0);
            set_network_status(CLOCK_NETWORK_CONNECTING);
            services_were_recovered = true;
            ESP_LOGW(TAG, "Network lost; starting reconnect burst");
        }

        if (clock_network_get_status() == CLOCK_NETWORK_ONLINE) {
            const bool reset_burst = retry.status != CLOCK_NETWORK_ONLINE;
            clock_network_retry_connected(&retry);
            set_retry_attempt(0);
            if (reset_burst) {
                ESP_LOGI(TAG, "Reconnect burst reset");
            }

            if (!clock_network_services_started()) {
                start_network_services(get_active_interface_locked(),
                                       services_were_recovered);
                if (!clock_network_services_started()) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                services_were_recovered = false;
            }

            if (!startup_result_shown) {
                show_result(get_active_interface_locked());
                startup_result_shown = true;
            }

            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (retry.status == CLOCK_NETWORK_ONLINE) {
            stop_network_services();
            clock_network_retry_disconnected(&retry, now_ms());
            set_retry_attempt(0);
            set_network_status(CLOCK_NETWORK_CONNECTING);
            services_were_recovered = true;
            ESP_LOGW(TAG, "Network lost; starting reconnect burst");
        }

        uint64_t current_ms = now_ms();
        if (retry.status == CLOCK_NETWORK_OFFLINE) {
            if (!clock_network_retry_cooldown_is_expired(&retry,
                                                         current_ms)) {
                const uint64_t remaining_ms =
                    retry.next_action_ms - current_ms;
                ulTaskNotifyTake(
                    pdTRUE,
                    pdMS_TO_TICKS(static_cast<uint32_t>(remaining_ms)));
                continue;
            }

            ESP_LOGI(TAG,
                     "Offline cooldown expired; starting reconnect burst");
            clock_network_retry_start_burst(&retry, current_ms);
            set_retry_attempt(0);
            set_network_status(CLOCK_NETWORK_CONNECTING);
        }

        current_ms = now_ms();
        if (clock_network_retry_attempt_is_due(&retry, current_ms)) {
            const uint8_t attempt =
                clock_network_retry_begin_attempt(&retry);
            set_retry_attempt(attempt);
            set_network_status(CLOCK_NETWORK_CONNECTING);

            const char *const target = reconnect_target_label(mode);
            ESP_LOGI(TAG,
                     "%s reconnect attempt %u/%u",
                     target,
                     static_cast<unsigned>(attempt),
                     static_cast<unsigned>(CLOCK_NETWORK_ATTEMPTS_PER_BURST));

            uint64_t attempt_deadline_ms = retry.next_action_ms;
            if (attempt == CLOCK_NETWORK_ATTEMPTS_PER_BURST) {
                /*
                 * Attempt 4 starts at the 60-second boundary and gets one
                 * normal bounded connection window.  Its failure transitions
                 * directly to cooldown; this deadline is not another retry
                 * boundary.
                 */
                attempt_deadline_ms =
                    now_ms() + CLOCK_NETWORK_RETRY_SLOT_MS;
            }

            if (perform_connection_attempt(mode, attempt_deadline_ms) ||
                clock_network_get_status() == CLOCK_NETWORK_ONLINE) {
                clock_network_retry_connected(&retry);
                set_retry_attempt(0);
                ESP_LOGI(TAG, "Reconnect burst reset");
                continue;
            }

            clock_network_retry_attempt_failed(&retry);
            if (attempt < CLOCK_NETWORK_ATTEMPTS_PER_BURST) {
                ESP_LOGW(TAG,
                         "%s reconnect attempt %u/%u failed; "
                         "remaining in reconnect burst",
                         target,
                         static_cast<unsigned>(attempt),
                         static_cast<unsigned>(
                             CLOCK_NETWORK_ATTEMPTS_PER_BURST));
            } else {
                ESP_LOGW(TAG,
                         "%s reconnect attempt %u/%u failed",
                         target,
                         static_cast<unsigned>(attempt),
                         static_cast<unsigned>(
                             CLOCK_NETWORK_ATTEMPTS_PER_BURST));
            }
            continue;
        }

        current_ms = now_ms();
        if (clock_network_retry_burst_is_exhausted(&retry, current_ms)) {
            if (clock_network_get_status() == CLOCK_NETWORK_ONLINE) {
                continue;
            }

            ESP_LOGW(TAG,
                     "Reconnect burst exhausted; entering %u s "
                     "offline cooldown",
                     static_cast<unsigned>(CLOCK_NETWORK_COOLDOWN_MS / 1000UL));
            stop_selected_interface();
            clock_network_retry_enter_cooldown(&retry, current_ms);
            set_retry_attempt(0);
            set_network_status(CLOCK_NETWORK_OFFLINE);

            if (!startup_result_shown) {
                show_result(CLOCK_NETWORK_INTERFACE_NONE);
                startup_result_shown = true;
            }
            continue;
        }

        const uint64_t remaining_ms = retry.next_action_ms - current_ms;
        ulTaskNotifyTake(pdTRUE,
                        pdMS_TO_TICKS(static_cast<uint32_t>(remaining_ms)));
    }
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
    set_retry_attempt(0);
    set_services_started(false);
    s_rx_callback = rx_callback;
    s_show_message = show_message;

    clock_ethernet_set_event_callback(
        network_interface_event,
        reinterpret_cast<void *>(
            static_cast<uintptr_t>(CLOCK_NETWORK_INTERFACE_ETHERNET)));
    clock_wifi_set_event_callback(
        network_interface_event,
        reinterpret_cast<void *>(
            static_cast<uintptr_t>(CLOCK_NETWORK_INTERFACE_WIFI)));

    set_network_status(CLOCK_NETWORK_CONNECTING);

    const BaseType_t result = xTaskCreatePinnedToCore(
        network_supervisor_task,
        "NetworkSupervisor",
        7168,
        nullptr,
        2,
        &s_supervisor_task,
        0);

    if (result != pdPASS) {
        clear_network_task_started();
        set_network_status(CLOCK_NETWORK_OFFLINE);
        ESP_LOGE(TAG, "Failed to create network supervisor task");
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

clock_network_status_t clock_network_get_status(void)
{
    int status;
    portENTER_CRITICAL(&s_state_lock);
    status = s_network_status;
    portEXIT_CRITICAL(&s_state_lock);
    return static_cast<clock_network_status_t>(status);
}

bool clock_network_services_started(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool started = s_services_started;
    portEXIT_CRITICAL(&s_state_lock);
    return started;
}
