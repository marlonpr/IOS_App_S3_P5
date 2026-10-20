#include "clock_wifi.h"

#include <string.h>

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "lwip/def.h"
#include "lwip/ip4_addr.h"

#include "clock_network.h"

namespace {

constexpr EventBits_t kWifiGotIpBit = BIT0;
constexpr EventBits_t kWifiAttemptFailedBit = BIT1;
constexpr EventBits_t kWifiStartedBit = BIT2;
constexpr uint32_t kRejectedNetworkRetryDelayMs = 1000;
const char *const TAG = "CLOCK_WIFI";

EventGroupHandle_t s_wifi_events = nullptr;
esp_netif_t *s_wifi_netif = nullptr;
TaskHandle_t s_dhcp_retry_task = nullptr;
std::atomic<bool> s_dhcp_retry_pending{false};
std::atomic<bool> s_rejected_lease_recovery{false};
std::atomic<uint32_t> s_rejected_network_count{0};
std::atomic<uint32_t> s_diagnostic_ipv4{0};
bool s_initialized = false;
bool s_power_save_configured = false;
clock_network_interface_event_callback_t s_event_callback = nullptr;
void *s_event_context = nullptr;

struct ipv4_acceptance_policy_t {
    uint32_t network = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    bool valid = false;
};

ipv4_acceptance_policy_t s_ipv4_policy;

bool parse_ipv4_config(const char *text, uint32_t *out_host_order)
{
    if (text == nullptr || out_host_order == nullptr) {
        return false;
    }

    ip4_addr_t parsed = {};
    if (ip4addr_aton(text, &parsed) == 0) {
        return false;
    }

    *out_host_order = lwip_ntohl(parsed.addr);
    return true;
}

bool load_ipv4_acceptance_policy()
{
    ipv4_acceptance_policy_t policy = {};
    if (!parse_ipv4_config(CONFIG_CLOCK_WIFI_EXPECTED_NETWORK, &policy.network) ||
        !parse_ipv4_config(CONFIG_CLOCK_WIFI_EXPECTED_NETMASK, &policy.netmask) ||
        !parse_ipv4_config(CONFIG_CLOCK_WIFI_EXPECTED_GATEWAY, &policy.gateway)) {
        ESP_LOGE(TAG,
                 "Invalid Wi-Fi LAN policy in menuconfig: network='%s' mask='%s' gateway='%s'",
                 CONFIG_CLOCK_WIFI_EXPECTED_NETWORK,
                 CONFIG_CLOCK_WIFI_EXPECTED_NETMASK,
                 CONFIG_CLOCK_WIFI_EXPECTED_GATEWAY);
        return false;
    }

    if ((policy.network & policy.netmask) != policy.network) {
        ESP_LOGE(TAG,
                 "Accepted Wi-Fi IPv4 network '%s' is not a network base for mask '%s'",
                 CONFIG_CLOCK_WIFI_EXPECTED_NETWORK,
                 CONFIG_CLOCK_WIFI_EXPECTED_NETMASK);
        return false;
    }

    if ((policy.gateway & policy.netmask) != policy.network) {
        ESP_LOGE(TAG,
                 "Accepted Wi-Fi gateway '%s' is outside network '%s' mask '%s'",
                 CONFIG_CLOCK_WIFI_EXPECTED_GATEWAY,
                 CONFIG_CLOCK_WIFI_EXPECTED_NETWORK,
                 CONFIG_CLOCK_WIFI_EXPECTED_NETMASK);
        return false;
    }

    policy.valid = true;
    s_ipv4_policy = policy;
    ESP_LOGI(TAG,
             "Accepted Wi-Fi LAN policy: network=%s mask=%s gateway=%s",
             CONFIG_CLOCK_WIFI_EXPECTED_NETWORK,
             CONFIG_CLOCK_WIFI_EXPECTED_NETMASK,
             CONFIG_CLOCK_WIFI_EXPECTED_GATEWAY);
    return true;
}

bool is_accepted_ip_info(const esp_netif_ip_info_t &info)
{
    return s_ipv4_policy.valid &&
           clock_network_ipv4_matches_policy(
               lwip_ntohl(info.ip.addr),
               lwip_ntohl(info.netmask.addr),
               lwip_ntohl(info.gw.addr),
               s_ipv4_policy.network,
               s_ipv4_policy.netmask,
               s_ipv4_policy.gateway);
}

void request_dhcp_retry()
{
    if (s_dhcp_retry_task == nullptr) {
        ESP_LOGE(TAG, "Cannot retry DHCP because the retry task is unavailable");
        return;
    }

    bool expected = false;
    if (s_dhcp_retry_pending.compare_exchange_strong(expected, true)) {
        xTaskNotifyGive(s_dhcp_retry_task);
    }
}

void dhcp_retry_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(kRejectedNetworkRetryDelayMs));

        if (s_wifi_netif == nullptr || s_wifi_events == nullptr ||
            (xEventGroupGetBits(s_wifi_events) & kWifiStartedBit) == 0) {
            ESP_LOGW(TAG,
                     "Rejected DHCP lease retry cancelled because Wi-Fi STA is not active");
            s_dhcp_retry_pending.store(false);
            s_rejected_lease_recovery.store(false);
            continue;
        }

        ESP_LOGW(TAG,
                 "Retrying DHCP after rejected network lease (attempt=%u)",
                 static_cast<unsigned>(s_rejected_network_count.load()));

        const esp_err_t stop_result = esp_netif_dhcpc_stop(s_wifi_netif);
        if (stop_result != ESP_OK) {
            ESP_LOGI(TAG, "DHCP client stop returned: %s",
                     esp_err_to_name(stop_result));
        }

        // Remove the rejected address while DHCP is stopped. kWifiGotIpBit was
        // never set for this lease, so the network supervisor cannot start TCP,
        // mDNS, logo upload, or OTA traffic on the wrong LAN.
        const esp_netif_ip_info_t empty_ip_info = {};
        const esp_err_t clear_result =
            esp_netif_set_ip_info(s_wifi_netif, &empty_ip_info);
        if (clear_result != ESP_OK) {
            ESP_LOGW(TAG, "Could not clear rejected IPv4 lease: %s",
                     esp_err_to_name(clear_result));
        }

        const esp_err_t start_result = esp_netif_dhcpc_start(s_wifi_netif);
        s_dhcp_retry_pending.store(false);
        if (start_result != ESP_OK) {
            ESP_LOGW(TAG, "DHCP client restart failed: %s; retrying",
                     esp_err_to_name(start_result));
            request_dhcp_retry();
        }
    }
}

void notify_network(clock_network_interface_event_t event, uint32_t detail = 0)
{
    if (s_event_callback != nullptr) {
        s_event_callback(event, detail, s_event_context);
    }
}

const char *disconnect_reason_description(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
            return "authentication expired";
        case WIFI_REASON_AUTH_FAIL:
            return "authentication failed";
        case WIFI_REASON_NO_AP_FOUND:
            return "access point not found";
        case WIFI_REASON_ASSOC_FAIL:
            return "association failed";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "beacon timeout";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "handshake timeout";
        default:
            return "unspecified/other";
    }
}

void configure_power_save()
{
    if (s_power_save_configured) {
        return;
    }

    const esp_err_t result = esp_wifi_set_ps(WIFI_PS_NONE);
    if (result == ESP_OK) {
        s_power_save_configured = true;
        ESP_LOGI(TAG, "Wi-Fi power save disabled for always-on operation");
    } else {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s",
                 esp_err_to_name(result));
    }
}

esp_err_t prepare_network_stack()
{
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    return ESP_OK;
}

void wifi_event_handler(void *,
                        esp_event_base_t event_base,
                        int32_t event_id,
                        void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupClearBits(s_wifi_events, kWifiAttemptFailedBit);
        xEventGroupSetBits(s_wifi_events, kWifiStartedBit);
        notify_network(CLOCK_NETWORK_INTERFACE_EVENT_STARTED);
        ESP_LOGI(TAG, "Wi-Fi station started; connecting");
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi connect request failed: %s",
                     esp_err_to_name(result));
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        const uint8_t reason = event == nullptr ? 0U : event->reason;
        xEventGroupClearBits(s_wifi_events, kWifiGotIpBit);
        xEventGroupSetBits(s_wifi_events, kWifiAttemptFailedBit);
        s_diagnostic_ipv4.store(0);
        s_rejected_lease_recovery.store(false);
        s_dhcp_retry_pending.store(false);
        ESP_LOGW(TAG,
                 "Wi-Fi disconnected: reason=%u (%s)",
                 static_cast<unsigned>(reason),
                 disconnect_reason_description(reason));
        notify_network(CLOCK_NETWORK_INTERFACE_EVENT_DISCONNECTED, reason);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        xEventGroupClearBits(s_wifi_events,
                             kWifiStartedBit | kWifiGotIpBit);
        s_diagnostic_ipv4.store(0);
        s_rejected_lease_recovery.store(false);
        s_dhcp_retry_pending.store(false);
        notify_network(CLOCK_NETWORK_INTERFACE_EVENT_STOPPED);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_wifi_events, kWifiGotIpBit);
        if (s_rejected_lease_recovery.load()) {
            ESP_LOGI(TAG,
                     "Ignoring expected lost-IP event while replacing a rejected DHCP lease");
            return;
        }

        s_diagnostic_ipv4.store(0);
        xEventGroupSetBits(s_wifi_events, kWifiAttemptFailedBit);
        ESP_LOGW(TAG, "Wi-Fi lost its local IP address");
        notify_network(CLOCK_NETWORK_INTERFACE_EVENT_LOST_IP);
        return;
    }

    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    if (event == nullptr) {
        return;
    }

    s_diagnostic_ipv4.store(event->ip_info.ip.addr);

    ESP_LOGI(TAG, "Wi-Fi got IP");
    ESP_LOGI(TAG, "WIFIIP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "WIFIMASK: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "WIFIGW: " IPSTR, IP2STR(&event->ip_info.gw));

    if (!is_accepted_ip_info(event->ip_info)) {
        xEventGroupClearBits(s_wifi_events, kWifiGotIpBit);
        s_rejected_lease_recovery.store(true);
        const uint32_t attempt = s_rejected_network_count.fetch_add(1) + 1;
        ESP_LOGW(TAG,
                 "Rejected DHCP lease: ip=" IPSTR " mask=" IPSTR " gw=" IPSTR
                 "; expected network=%s mask=%s gateway=%s (attempt=%u)",
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw),
                 CONFIG_CLOCK_WIFI_EXPECTED_NETWORK,
                 CONFIG_CLOCK_WIFI_EXPECTED_NETMASK,
                 CONFIG_CLOCK_WIFI_EXPECTED_GATEWAY,
                 static_cast<unsigned>(attempt));
        ESP_LOGW(TAG,
                 "Network services remain disabled; requesting a fresh DHCP lease");
        request_dhcp_retry();
        return;
    }

    s_rejected_network_count.store(0);
    s_rejected_lease_recovery.store(false);
    ESP_LOGI(TAG,
             "Accepted factory LAN: ip=" IPSTR " mask=" IPSTR " gw=" IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));

    esp_netif_dns_info_t dns_info = {};
    if (s_wifi_netif != nullptr &&
        esp_netif_get_dns_info(s_wifi_netif,
                               ESP_NETIF_DNS_MAIN,
                               &dns_info) == ESP_OK &&
        dns_info.ip.type == ESP_IPADDR_TYPE_V4 &&
        dns_info.ip.u_addr.ip4.addr != 0) {
        ESP_LOGI(TAG, "WIFIDNS: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "Wi-Fi DNS is not ready");
    }

    xEventGroupClearBits(s_wifi_events, kWifiAttemptFailedBit);
    xEventGroupSetBits(s_wifi_events, kWifiGotIpBit);
    notify_network(CLOCK_NETWORK_INTERFACE_EVENT_GOT_IP,
                   event->ip_info.ip.addr);
}

}  // namespace

esp_err_t clock_wifi_init_sta(void)
{
    if (s_initialized) {
        xEventGroupClearBits(s_wifi_events,
                             kWifiGotIpBit | kWifiAttemptFailedBit);

        if ((xEventGroupGetBits(s_wifi_events) & kWifiStartedBit) != 0) {
            ESP_LOGI(TAG, "Requesting another Wi-Fi connection attempt");
            return esp_wifi_connect();
        }

        ESP_LOGI(TAG, "Restarting Wi-Fi STA for another connection attempt");
        const esp_err_t restart_result = esp_wifi_start();
        if (restart_result == ESP_OK) {
            configure_power_save();
        }
        return restart_result;
    }

    if (CONFIG_ESP_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG,
                 "Wi-Fi SSID is empty; configure CONFIG_ESP_WIFI_SSID in menuconfig");
        return ESP_ERR_INVALID_ARG;
    }

    if (!load_ipv4_acceptance_policy()) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = prepare_network_stack();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi network stack init failed: %s",
                 esp_err_to_name(result));
        return result;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == nullptr) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi station netif");
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(result));
        return result;
    }

    result = esp_event_handler_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        nullptr);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_register(IP_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        nullptr);
    if (result != ESP_OK) {
        return result;
    }

    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid),
            CONFIG_ESP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.password),
            CONFIG_ESP_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (result != ESP_OK) {
        return result;
    }

    if (xTaskCreate(&dhcp_retry_task,
                    "wifi_dhcp_retry",
                    3072,
                    nullptr,
                    4,
                    &s_dhcp_retry_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi DHCP retry task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Starting Wi-Fi STA");
    ESP_LOGI(TAG, "Wi-Fi SSID: %s", CONFIG_ESP_WIFI_SSID);
    ESP_LOGI(TAG, "Wi-Fi connecting");

    result = esp_wifi_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(result));
    } else {
        configure_power_save();
    }

    return result;
}

bool clock_wifi_wait_for_ip(uint32_t timeout_ms)
{
    if (s_wifi_events == nullptr) {
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        kWifiGotIpBit | kWifiAttemptFailedBit,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if ((bits & kWifiGotIpBit) == 0) {
        if ((bits & kWifiAttemptFailedBit) != 0) {
            ESP_LOGW(TAG, "Wi-Fi connection attempt ended without a local IP");
        } else {
            ESP_LOGW(TAG, "Wi-Fi connection timed out after %u ms",
                     static_cast<unsigned>(timeout_ms));
        }
        return false;
    }

    return true;
}

bool clock_wifi_route_is_ready(void)
{
    if (s_wifi_netif == nullptr) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    esp_netif_dns_info_t dns_info = {};

    return esp_netif_get_ip_info(s_wifi_netif, &ip_info) == ESP_OK &&
           ip_info.ip.addr != 0 &&
           ip_info.gw.addr != 0 &&
           esp_netif_get_dns_info(s_wifi_netif,
                                  ESP_NETIF_DNS_MAIN,
                                  &dns_info) == ESP_OK &&
           dns_info.ip.type == ESP_IPADDR_TYPE_V4 &&
           dns_info.ip.u_addr.ip4.addr != 0;
}

bool clock_wifi_get_diagnostic_ipv4(uint32_t *address)
{
    if (address == nullptr) {
        return false;
    }

    const uint32_t current = s_diagnostic_ipv4.load();
    *address = current;
    return current != 0;
}

esp_err_t clock_wifi_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    xEventGroupClearBits(s_wifi_events,
                         kWifiGotIpBit | kWifiAttemptFailedBit);

    const esp_err_t result = esp_wifi_stop();
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi stopped");
    } else {
        ESP_LOGW(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(result));
    }
    return result;
}

void clock_wifi_set_event_callback(
    clock_network_interface_event_callback_t callback,
    void *context)
{
    s_event_callback = callback;
    s_event_context = context;
}
