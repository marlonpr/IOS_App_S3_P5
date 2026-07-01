#include "clock_sd_card.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"

static const char *TAG = "CLOCK_SD_CARD";

static constexpr spi_host_device_t kSdSpiHost = SPI2_HOST;
static constexpr int kSdMaxTransferSize = 16 * 1024;
static constexpr int kSdFrequencyKhz = 20000;

static bool s_sd_card_mounted = false;
static sdmmc_card_t *s_sd_card = nullptr;

esp_err_t clock_sd_card_init()
{
    if (s_sd_card_mounted) {
        ESP_LOGI(TAG, "SD card is already mounted");
        return ESP_OK;
    }

    static constexpr const char *kMountPoint = "/sdcard";

    spi_bus_config_t bus_config = {};

    bus_config.mosi_io_num = GPIO_NUM_6;
    bus_config.miso_io_num = GPIO_NUM_5;
    bus_config.sclk_io_num = GPIO_NUM_7;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = kSdMaxTransferSize;

    esp_err_t err = spi_bus_initialize(
        kSdSpiHost,
        &bus_config,
        SPI_DMA_CH_AUTO
    );

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD SPI bus initialized");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SD SPI bus already initialized");
    } else {
        ESP_LOGE(
            TAG,
            "SD initialization failed: spi_bus_initialize failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    host.slot = kSdSpiHost;
    host.max_freq_khz = kSdFrequencyKhz;

    sdspi_device_config_t slot_config =
        SDSPI_DEVICE_CONFIG_DEFAULT();

    slot_config.host_id = kSdSpiHost;
    slot_config.gpio_cs = GPIO_NUM_4;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};

    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;
    mount_config.disk_status_check_enable = false;
    mount_config.use_one_fat = false;

    err = esp_vfs_fat_sdspi_mount(
        kMountPoint,
        &host,
        &slot_config,
        &mount_config,
        &s_sd_card
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD initialization failed: mount failed: %s",
            esp_err_to_name(err)
        );

        s_sd_card = nullptr;
        s_sd_card_mounted = false;

        return err;
    }

    s_sd_card_mounted = true;

    ESP_LOGI(TAG, "SD card mounted at %s", kMountPoint);

    sdmmc_card_print_info(stdout, s_sd_card);

    return ESP_OK;
}

bool clock_sd_card_is_mounted()
{
    return s_sd_card_mounted;
}