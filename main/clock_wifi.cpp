#include "clock_wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"

namespace {

constexpr EventBits_t kWifiGotIpBit = BIT0;
const char *const TAG = "CLOCK_WIFI";

EventGroupHandle_t s_wifi_events = nullptr;
esp_netif_t *s_wifi_netif = nullptr;
bool s_initialized = false;
bool s_allow_reconnect = false;
uint32_t s_retry_count = 0;

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
        xEventGroupClearBits(s_wifi_events, kWifiGotIpBit);
        ESP_LOGW(TAG,
                 "Wi-Fi disconnected (reason=%u)",
                 event == nullptr ? 0U : static_cast<unsigned>(event->reason));

        if (s_allow_reconnect) {
            ++s_retry_count;
            ESP_LOGI(TAG, "Wi-Fi reconnect attempt %u",
                     static_cast<unsigned>(s_retry_count));
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi reconnect request failed: %s",
                         esp_err_to_name(result));
            }
        }
        return;
    }

    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    if (event == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi got IP");
    ESP_LOGI(TAG, "WIFIIP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "WIFIMASK: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "WIFIGW: " IPSTR, IP2STR(&event->ip_info.gw));

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

    s_retry_count = 0;
    xEventGroupSetBits(s_wifi_events, kWifiGotIpBit);
}

}  // namespace

esp_err_t clock_wifi_init_sta(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (CONFIG_ESP_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG,
                 "Wi-Fi SSID is empty; configure CONFIG_ESP_WIFI_SSID in menuconfig");
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
                                        IP_EVENT_STA_GOT_IP,
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

    s_allow_reconnect = true;
    s_retry_count = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "Starting Wi-Fi STA");
    ESP_LOGI(TAG, "Wi-Fi SSID: %s", CONFIG_ESP_WIFI_SSID);
    ESP_LOGI(TAG, "Wi-Fi connecting");

    result = esp_wifi_start();
    if (result != ESP_OK) {
        s_allow_reconnect = false;
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(result));
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
        kWifiGotIpBit,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if ((bits & kWifiGotIpBit) == 0) {
        ESP_LOGW(TAG, "Wi-Fi connection timed out after %u ms",
                 static_cast<unsigned>(timeout_ms));
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

esp_err_t clock_wifi_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_allow_reconnect = false;
    xEventGroupClearBits(s_wifi_events, kWifiGotIpBit);

    const esp_err_t result = esp_wifi_stop();
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi stopped");
    } else {
        ESP_LOGW(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(result));
    }
    return result;
}
