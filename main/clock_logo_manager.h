#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_LOGO_WIDTH 64
#define CLOCK_LOGO_HEIGHT 32
#define CLOCK_LOGO_PIXEL_COUNT 2048
#define CLOCK_LOGO_PAYLOAD_BYTES 6144
#define CLOCK_LOGO_HEADER_BYTES 20
#define CLOCK_LOGO_SEQUENCE_FRAME_COUNT 5
#define CLOCK_LOGO_LEGACY_SEQUENCE_FRAME_COUNT 4
#define CLOCK_LOGO_SEQUENCE_FRAME_DURATION_MS 3000
#define CLOCK_LOGO_SEQUENCE_PAYLOAD_BYTES \
    (CLOCK_LOGO_PAYLOAD_BYTES * CLOCK_LOGO_SEQUENCE_FRAME_COUNT)

typedef struct {
    uint8_t frame_count;
    uint16_t frame_duration_ms;
    uint32_t payload_length;
    uint32_t payload_crc;
} clock_logo_file_info_t;

void clock_logo_init();

const uint32_t *clock_logo_lock_pixels();
void clock_logo_unlock_pixels();
void clock_logo_restart_sequence();

esp_err_t clock_logo_reload_from_sd();

bool clock_logo_parse_file_header(
    const uint8_t header[CLOCK_LOGO_HEADER_BYTES],
    clock_logo_file_info_t *info);

uint32_t clock_logo_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t clock_logo_crc32_finalize(uint32_t crc);

typedef enum {
    CLOCK_LOGO_RESTORE_OK = 0,
    CLOCK_LOGO_RESTORE_BUSY,
    CLOCK_LOGO_RESTORE_STORAGE_ERROR,
} clock_logo_restore_result_t;

/*
 * Shared operation lock used to serialize:
 *
 * - logo upload
 * - logo file replacement
 * - restoring the compiled default logo
 */
bool clock_logo_operation_begin(uint32_t timeout_ms);
void clock_logo_operation_end();

/*
 * Permanently removes the SD-card logo and activates the compiled logo.
 */
clock_logo_restore_result_t clock_logo_restore_compiled_default();

#ifdef __cplusplus
}
#endif
