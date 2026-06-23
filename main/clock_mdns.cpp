#include "clock_mdns.h"

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "CLOCK_MDNS";

static bool s_mdns_initialized = false;
static bool s_service_added = false;

static char s_hostname[32];
static char s_instance_name[48];
static char s_board_id_text[8];

esp_err_t clock_mdns_start(uint8_t board_id)
{
    if (s_service_added) {
        return ESP_OK;
    }

    /*
     * The hostname becomes:
     *
     *     esp32-clock-0.local
     *
     * Each board must use a unique board ID.
     */
    snprintf(
        s_hostname,
        sizeof(s_hostname),
        "esp32-clock-%u",
        static_cast<unsigned>(board_id)
    );

    snprintf(
        s_instance_name,
        sizeof(s_instance_name),
        "ESP32 Clock %u",
        static_cast<unsigned>(board_id)
    );

    snprintf(
        s_board_id_text,
        sizeof(s_board_id_text),
        "%u",
        static_cast<unsigned>(board_id)
    );

    if (!s_mdns_initialized) {
        esp_err_t err = mdns_init();

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "mdns_init failed: %s",
                esp_err_to_name(err)
            );

            return err;
        }

        s_mdns_initialized = true;
    }

    esp_err_t err = mdns_hostname_set(s_hostname);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "mdns_hostname_set failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = mdns_instance_name_set(s_instance_name);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "mdns_instance_name_set failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    const esp_app_desc_t *app_description =
        esp_app_get_description();

    mdns_txt_item_t txt_items[] = {
        {"id", s_board_id_text},
        {"model", "ESP32-S3-ETH"},
        {"protocol", "TA1"},
        {"firmware", app_description->version}
    };

    err = mdns_service_add(
        s_instance_name,
        "_espclock",
        "_tcp",
        5000,
        txt_items,
        sizeof(txt_items) / sizeof(txt_items[0])
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "mdns_service_add failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    s_service_added = true;

    ESP_LOGI(TAG, "mDNS service advertised");
    ESP_LOGI(TAG, "Hostname: %s.local", s_hostname);
    ESP_LOGI(TAG, "Instance: %s", s_instance_name);
    ESP_LOGI(TAG, "Service: _espclock._tcp.local");
    ESP_LOGI(TAG, "Port: 5000");
    ESP_LOGI(TAG, "Board ID: %u", board_id);

    return ESP_OK;
}