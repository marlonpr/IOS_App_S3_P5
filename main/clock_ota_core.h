#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_OTA_VERSION_MAX_LENGTH 31
#define CLOCK_OTA_URL_MAX_LENGTH     511

typedef struct {
    char version[CLOCK_OTA_VERSION_MAX_LENGTH + 1];
    char url[CLOCK_OTA_URL_MAX_LENGTH + 1];
} clock_ota_manifest_t;

typedef enum {
    CLOCK_OTA_START_ACCEPTED = 0,
    CLOCK_OTA_START_INVALID_URL,
    CLOCK_OTA_START_ALREADY_IN_PROGRESS,
} clock_ota_start_result_t;

/*
 * Parse one JSON object containing string-valued "version" and "url" fields.
 * Unknown JSON fields are allowed. Missing, duplicate, non-string, empty, or
 * oversized required fields are rejected.
 */
bool clock_ota_parse_version_json(const char *json,
                                  size_t json_length,
                                  clock_ota_manifest_t *manifest);

/* Firmware versions intentionally use exact string equality. */
bool clock_ota_versions_equal(const char *current_version,
                              const char *available_version);

/* Only non-empty HTTP and HTTPS URLs are accepted. */
bool clock_ota_url_is_supported(const char *url);

/*
 * Atomically validate a URL and reserve the single OTA operation slot.
 * This small state gate is shared by the GitHub check and URL-based OTA path.
 */
clock_ota_start_result_t clock_ota_try_reserve_start(const char *url);
void clock_ota_release_start(void);

#ifdef __cplusplus
}
#endif
