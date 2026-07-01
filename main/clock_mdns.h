#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t clock_mdns_start(uint8_t board_id);
esp_err_t clock_mdns_add_logo_upload_service(uint8_t board_id);
