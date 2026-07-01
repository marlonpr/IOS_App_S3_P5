#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t logo_upload_server_start(void);
esp_err_t logo_upload_server_register_mdns_service(uint8_t board_id);

#ifdef __cplusplus
}
#endif
