#pragma once

#include "esp_err.h"

#ifndef CLOCK_OTA_VERSION_JSON_URL
#define CLOCK_OTA_VERSION_JSON_URL \
    "https://github.com/marlonpr/IOS_App_S3_P5/releases/latest/download/version.json"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Confirm a newly booted OTA image when rollback support is enabled. */
void clock_ota_confirm_app_if_needed(void);

/* Log the running application version and partition. */
void clock_ota_print_app_info(void);

/* Launch the delayed GitHub version check no more than once per boot. */
esp_err_t clock_ota_start_github_check_once(void);

/* Launch an OTA task using a task-owned copy of an HTTP(S) URL. */
esp_err_t clock_ota_start_from_url(const char *url);

#ifdef __cplusplus
}
#endif
