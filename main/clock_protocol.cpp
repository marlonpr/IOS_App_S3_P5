#include "clock_protocol.h"

#include "esp_log.h"
#include "esp_err.h"

#include "clock_alarm.h"
#include "clock_menu.h"
#include "clock_palette.h"

#include "clock_logo_manager.h"
#include "clock_modes.h"

static const char *TAG = "CLOCK_PROTOCOL";

static clock_protocol_context_t s_ctx = {};

void clock_protocol_init(const clock_protocol_context_t *ctx)
{
    if (ctx != NULL) {
        s_ctx = *ctx;
    }
}


static uint8_t bcd_to_dec(uint8_t bcd)
{
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static bool bcd_is_valid(uint8_t bcd)
{
    return ((bcd & 0x0F) <= 9) && (((bcd >> 4) & 0x0F) <= 9);
}



static int ethernet_intensity_to_level(uint8_t intensity)
{
    /*
     * Ethernet software sends intensity 0-255.
     * Local clock menu uses brightness level 1-10.
     */

    if (intensity < 1) {
        intensity = 1;
    }

    int level = ((int)intensity * 10 + 254) / 255;

    if (level < 1) {
        level = 1;
    }

    if (level > 10) {
        level = 10;
    }

    return level;
}

static uint8_t brightness_level_to_protocol_intensity(int level)
{
    if (level < 1) {
        level = 1;
    }

    if (level > 10) {
        level = 10;
    }

    return (uint8_t)((level * 255) / 10);
}

bool rtc_time_is_valid(const ds3231_time_t *time)
{
    if (!time) {
        return false;
    }

    if (time->year < 2025 || time->year > 2099) {
        return false;
    }

    if (time->month < 1 || time->month > 12) {
        return false;
    }

    int max_day = clock_menu_days_in_month(time->month, time->year);

    if (time->day < 1 || time->day > max_day) {
        return false;
    }

    if (time->hour > 23) {
        return false;
    }

    if (time->minute > 59) {
        return false;
    }

    if (time->second > 59) {
        return false;
    }

    if (time->day_of_week < 1 || time->day_of_week > 7) {
        return false;
    }

    return true;
}






#include <cstddef>
#include <cstdint>

static bool is_ascii_hex(uint8_t value)
{
    return
        (value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'F') ||
        (value >= 'a' && value <= 'f');
}

static constexpr uint8_t kPaletteProtocolMaxRoles = 16;

typedef enum {
    PALETTE_STATUS_SUCCESS = 0x00,
    PALETTE_STATUS_UNSUPPORTED_MODE = 0x01,
    PALETTE_STATUS_UNSUPPORTED_VERSION = 0x02,
    PALETTE_STATUS_INVALID_COUNT_OR_LENGTH = 0x03,
    PALETTE_STATUS_DUPLICATE_ROLE = 0x04,
    PALETTE_STATUS_UNSUPPORTED_ROLE = 0x05,
    PALETTE_STATUS_INCOMPLETE_ROLE_SET = 0x06,
    PALETTE_STATUS_INVALID_HEX = 0x07,
    PALETTE_STATUS_NVS_FAILURE = 0x08,
    PALETTE_STATUS_BUSY = 0x09,
    PALETTE_STATUS_INTERNAL_FAILURE = 0x0A,
} palette_protocol_status_t;

typedef enum {
    SET_MODE_STATUS_SUCCESS = 0x00,
    SET_MODE_STATUS_UNSUPPORTED_MODE = 0x01,
    SET_MODE_STATUS_INVALID_LENGTH = 0x02,
    SET_MODE_STATUS_INVALID_HEX = 0x03,
    SET_MODE_STATUS_NVS_FAILURE = 0x04,
    SET_MODE_STATUS_INTERNAL_FAILURE = 0x0A,
} set_mode_protocol_status_t;

static int ascii_hex_nibble(uint8_t value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }

    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }

    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }

    return -1;
}

static bool decode_ascii_hex_u8(const uint8_t *input, uint8_t *out_value)
{
    if (input == nullptr || out_value == nullptr) {
        return false;
    }

    int high = ascii_hex_nibble(input[0]);
    int low = ascii_hex_nibble(input[1]);

    if (high < 0 || low < 0) {
        return false;
    }

    *out_value = (uint8_t)((high << 4) | low);
    return true;
}

static void encode_ascii_hex_u8(uint8_t value, uint8_t *output)
{
    static constexpr char kHexDigits[] = "0123456789ABCDEF";

    output[0] = (uint8_t)kHexDigits[value >> 4];
    output[1] = (uint8_t)kHexDigits[value & 0x0F];
}

static bool is_set_mode_command(const uint8_t *p, int len)
{
    return p != nullptr && len >= 6 &&
           p[0] == '/' && p[1] == 'T' && p[2] == 'A' &&
           p[4] == 'S' && p[5] == 'M';
}

static int build_set_mode_response(uint8_t *tx,
                                   int tx_max,
                                   uint8_t board_id,
                                   uint8_t mode,
                                   set_mode_protocol_status_t status)
{
    static constexpr size_t kResponseLength = 11;

    if (tx == nullptr || tx_max < 0 ||
        (size_t)tx_max < kResponseLength) {
        ESP_LOGE(TAG, "TX buffer too small for SM response");
        return -1;
    }

    tx[0] = '/';
    tx[1] = 't';
    tx[2] = 'a';
    tx[3] = board_id;
    tx[4] = 's';
    tx[5] = 'm';
    encode_ascii_hex_u8(mode, &tx[6]);
    encode_ascii_hex_u8((uint8_t)status, &tx[8]);
    tx[10] = '\\';
    return (int)kResponseLength;
}

static set_mode_protocol_status_t map_set_mode_result(esp_err_t result)
{
    if (result == ESP_OK) {
        return SET_MODE_STATUS_SUCCESS;
    }

    if (result == ESP_ERR_INVALID_ARG ||
        result == ESP_ERR_INVALID_STATE) {
        return SET_MODE_STATUS_INTERNAL_FAILURE;
    }

    return SET_MODE_STATUS_NVS_FAILURE;
}

static int handle_set_mode_command(const uint8_t *p,
                                   int len,
                                   uint8_t *tx,
                                   int tx_max)
{
    const uint8_t board_id = p[3];
    uint8_t mode = 0;

    if (board_id != 0x00 || board_id == 0x5C) {
        ESP_LOGW(TAG,
                 "Ignoring SM command for invalid board ID: %u",
                 board_id);
        return -1;
    }

    if (tx == nullptr || tx_max < 11) {
        ESP_LOGE(TAG, "TX buffer too small for SM response");
        return -1;
    }

    const bool mode_is_parseable =
        len >= 8 && decode_ascii_hex_u8(&p[6], &mode);
    set_mode_protocol_status_t status = SET_MODE_STATUS_SUCCESS;

    if (len != 9 || p[8] != '\\') {
        status = SET_MODE_STATUS_INVALID_LENGTH;
    } else if (!mode_is_parseable) {
        status = SET_MODE_STATUS_INVALID_HEX;
    } else if (mode < MODE_1 || mode > MODE_ROTATION) {
        status = SET_MODE_STATUS_UNSUPPORTED_MODE;
    }

    if (status == SET_MODE_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "SM set mode request: mode=%u", mode);
        status = map_set_mode_result(clock_modes_set_mode(mode));
    }

    if (status == SET_MODE_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "SM set mode applied: mode=%u", mode);
    } else {
        ESP_LOGW(TAG,
                 "SM set mode rejected: mode=%u status=%02X",
                 mode,
                 status);
    }

    return build_set_mode_response(tx,
                                   tx_max,
                                   board_id,
                                   mode,
                                   status);
}

static bool palette_tx_has_capacity(uint8_t *tx,
                                    int tx_max,
                                    size_t required)
{
    return tx != nullptr && tx_max >= 0 && (size_t)tx_max >= required;
}

static void write_palette_response_prefix(uint8_t *tx,
                                          uint8_t board_id,
                                          uint8_t command_1,
                                          uint8_t command_2)
{
    tx[0] = '/';
    tx[1] = 't';
    tx[2] = 'a';
    tx[3] = board_id;
    tx[4] = command_1;
    tx[5] = command_2;
}

static int build_palette_ack(uint8_t *tx,
                             int tx_max,
                             uint8_t board_id,
                             uint8_t command_1,
                             uint8_t command_2,
                             uint8_t mode,
                             palette_protocol_status_t status)
{
    static constexpr size_t kAckLength = 11;

    if (!palette_tx_has_capacity(tx, tx_max, kAckLength)) {
        ESP_LOGE(TAG, "TX buffer too small for palette ACK");
        return -1;
    }

    write_palette_response_prefix(tx,
                                  board_id,
                                  command_1,
                                  command_2);
    encode_ascii_hex_u8(mode, &tx[6]);
    encode_ascii_hex_u8((uint8_t)status, &tx[8]);
    tx[10] = '\\';
    return (int)kAckLength;
}

static int build_lp_error_response(uint8_t *tx,
                                   int tx_max,
                                   uint8_t board_id,
                                   uint8_t mode,
                                   palette_protocol_status_t status)
{
    static constexpr size_t kErrorLength = 15;

    if (!palette_tx_has_capacity(tx, tx_max, kErrorLength)) {
        ESP_LOGE(TAG, "TX buffer too small for LP error response");
        return -1;
    }

    write_palette_response_prefix(tx, board_id, 'l', 'p');
    encode_ascii_hex_u8(mode, &tx[6]);
    encode_ascii_hex_u8((uint8_t)status, &tx[8]);
    encode_ascii_hex_u8(0, &tx[10]);
    encode_ascii_hex_u8(0, &tx[12]);
    tx[14] = '\\';
    return (int)kErrorLength;
}

static void sort_palette_entries(clock_mode_palette_t *palette)
{
    if (palette == nullptr || palette->count > CLOCK_PALETTE_MAX_ROLES) {
        return;
    }

    for (uint8_t i = 1; i < palette->count; ++i) {
        clock_palette_entry_t entry = palette->entries[i];
        uint8_t position = i;

        while (position > 0 &&
               palette->entries[position - 1].role > entry.role) {
            palette->entries[position] = palette->entries[position - 1];
            position--;
        }

        palette->entries[position] = entry;
    }
}

static bool palette_snapshot_is_complete(
    const clock_mode_palette_t *palette)
{
    if (palette == nullptr ||
        !clock_palette_is_supported_mode(palette->mode) ||
        palette->count == 0 ||
        palette->count > CLOCK_PALETTE_MAX_ROLES) {
        return false;
    }

    clock_mode_palette_t factory = {};

    if (!clock_palette_get_factory_snapshot(palette->mode, &factory) ||
        palette->count != factory.count) {
        return false;
    }

    for (uint8_t i = 0; i < palette->count; ++i) {
        uint8_t role = palette->entries[i].role;

        if (!clock_palette_is_supported_role(palette->mode, role)) {
            return false;
        }

        for (uint8_t previous = 0; previous < i; ++previous) {
            if (palette->entries[previous].role == role) {
                return false;
            }
        }
    }

    return true;
}

static int build_lp_success_response(uint8_t *tx,
                                     int tx_max,
                                     uint8_t board_id,
                                     uint8_t mode,
                                     const clock_mode_palette_t *palette)
{
    if (!palette_snapshot_is_complete(palette)) {
        return build_lp_error_response(tx,
                                       tx_max,
                                       board_id,
                                       mode,
                                       PALETTE_STATUS_INTERNAL_FAILURE);
    }

    clock_mode_palette_t sorted = *palette;
    sort_palette_entries(&sorted);

    size_t response_length = 15 + ((size_t)sorted.count * 8);

    if (!palette_tx_has_capacity(tx, tx_max, response_length)) {
        ESP_LOGE(TAG, "TX buffer too small for LP success response");
        return -1;
    }

    write_palette_response_prefix(tx, board_id, 'l', 'p');
    encode_ascii_hex_u8(mode, &tx[6]);
    encode_ascii_hex_u8(PALETTE_STATUS_SUCCESS, &tx[8]);
    encode_ascii_hex_u8(CLOCK_PALETTE_SCHEMA_VERSION, &tx[10]);
    encode_ascii_hex_u8(sorted.count, &tx[12]);

    size_t offset = 14;

    for (uint8_t i = 0; i < sorted.count; ++i) {
        const clock_palette_entry_t *entry = &sorted.entries[i];

        encode_ascii_hex_u8(entry->role, &tx[offset]);
        encode_ascii_hex_u8(entry->color.r, &tx[offset + 2]);
        encode_ascii_hex_u8(entry->color.g, &tx[offset + 4]);
        encode_ascii_hex_u8(entry->color.b, &tx[offset + 6]);
        offset += 8;
    }

    tx[offset] = '\\';
    return (int)response_length;
}

static palette_protocol_status_t map_palette_result(esp_err_t result)
{
    if (result == ESP_OK) {
        return PALETTE_STATUS_SUCCESS;
    }

    if (result == ESP_ERR_TIMEOUT) {
        return PALETTE_STATUS_BUSY;
    }

    if (result == ESP_ERR_INVALID_ARG ||
        result == ESP_ERR_INVALID_SIZE ||
        result == ESP_ERR_INVALID_STATE) {
        return PALETTE_STATUS_INTERNAL_FAILURE;
    }

    return PALETTE_STATUS_NVS_FAILURE;
}

static bool is_palette_command(const uint8_t *p, int len)
{
    if (p == nullptr || len < 6 ||
        p[0] != '/' || p[1] != 'T' || p[2] != 'A') {
        return false;
    }

    return (p[4] == 'L' && p[5] == 'P') ||
           (p[4] == 'C' && p[5] == 'P') ||
           (p[4] == 'D' && p[5] == 'P');
}

static int handle_lp_command(const uint8_t *p,
                             int len,
                             uint8_t *tx,
                             int tx_max)
{
    uint8_t board_id = p[3];
    uint8_t mode = 0;

    if (board_id != 0x00 || board_id == 0x5C) {
        ESP_LOGW(TAG, "Ignoring LP command for invalid board ID: %u", board_id);
        return -1;
    }

    palette_protocol_status_t status = PALETTE_STATUS_SUCCESS;

    if (len < 8) {
        status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
    } else if (!decode_ascii_hex_u8(&p[6], &mode)) {
        status = PALETTE_STATUS_INVALID_HEX;
    } else if (!clock_palette_is_supported_mode(mode)) {
        status = PALETTE_STATUS_UNSUPPORTED_MODE;
    } else if (len != 9 || p[8] != '\\') {
        status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
    }

    if (status != PALETTE_STATUS_SUCCESS) {
        ESP_LOGW(TAG,
                 "LP palette request rejected: mode=%u status=%02X",
                 mode,
                 status);
        return build_lp_error_response(tx,
                                       tx_max,
                                       board_id,
                                       mode,
                                       status);
    }

    ESP_LOGI(TAG, "LP palette request: mode=%u", mode);

    clock_mode_palette_t palette = {};

    if (!clock_palette_get_mode_snapshot(mode, &palette)) {
        return build_lp_error_response(tx,
                                       tx_max,
                                       board_id,
                                       mode,
                                       PALETTE_STATUS_INTERNAL_FAILURE);
    }

    return build_lp_success_response(tx,
                                     tx_max,
                                     board_id,
                                     mode,
                                     &palette);
}

static int handle_cp_command(const uint8_t *p,
                             int len,
                             uint8_t *tx,
                             int tx_max)
{
    uint8_t board_id = p[3];
    uint8_t mode = 0;
    uint8_t version = 0;
    uint8_t count = 0;
    palette_protocol_status_t status = PALETTE_STATUS_SUCCESS;

    if (board_id != 0x00 || board_id == 0x5C) {
        ESP_LOGW(TAG, "Ignoring CP command for invalid board ID: %u", board_id);
        return -1;
    }

    if (len < 8) {
        status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
    } else if (!decode_ascii_hex_u8(&p[6], &mode)) {
        status = PALETTE_STATUS_INVALID_HEX;
    } else if (!clock_palette_is_supported_mode(mode)) {
        status = PALETTE_STATUS_UNSUPPORTED_MODE;
    } else if (len < 13 || p[len - 1] != '\\') {
        status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
    } else if (!decode_ascii_hex_u8(&p[8], &version) ||
               !decode_ascii_hex_u8(&p[10], &count)) {
        status = PALETTE_STATUS_INVALID_HEX;
    } else if (version != CLOCK_PALETTE_SCHEMA_VERSION) {
        status = PALETTE_STATUS_UNSUPPORTED_VERSION;
    } else if (count == 0 || count > kPaletteProtocolMaxRoles) {
        status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
    } else {
        size_t expected_length = 13 + ((size_t)count * 8);

        if ((size_t)len != expected_length) {
            status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
        }
    }

    clock_palette_entry_t entries[kPaletteProtocolMaxRoles] = {};

    if (status == PALETTE_STATUS_SUCCESS) {
        for (uint8_t i = 0; i < count; ++i) {
            size_t offset = 12 + ((size_t)i * 8);

            if (!decode_ascii_hex_u8(&p[offset], &entries[i].role) ||
                !decode_ascii_hex_u8(&p[offset + 2], &entries[i].color.r) ||
                !decode_ascii_hex_u8(&p[offset + 4], &entries[i].color.g) ||
                !decode_ascii_hex_u8(&p[offset + 6], &entries[i].color.b)) {
                status = PALETTE_STATUS_INVALID_HEX;
                break;
            }
        }
    }

    if (status == PALETTE_STATUS_SUCCESS) {
        for (uint8_t i = 0; i < count; ++i) {
            for (uint8_t previous = 0; previous < i; ++previous) {
                if (entries[previous].role == entries[i].role) {
                    status = PALETTE_STATUS_DUPLICATE_ROLE;
                    break;
                }
            }

            if (status != PALETTE_STATUS_SUCCESS) {
                break;
            }
        }
    }

    if (status == PALETTE_STATUS_SUCCESS) {
        for (uint8_t i = 0; i < count; ++i) {
            if (!clock_palette_is_supported_role(mode, entries[i].role)) {
                status = PALETTE_STATUS_UNSUPPORTED_ROLE;
                break;
            }
        }
    }

    if (status == PALETTE_STATUS_SUCCESS) {
        clock_mode_palette_t factory = {};

        if (!clock_palette_get_factory_snapshot(mode, &factory)) {
            status = PALETTE_STATUS_INTERNAL_FAILURE;
        } else if (count != factory.count) {
            status = PALETTE_STATUS_INCOMPLETE_ROLE_SET;
        } else {
            for (uint8_t required = 0; required < factory.count; ++required) {
                bool found = false;

                for (uint8_t i = 0; i < count; ++i) {
                    if (entries[i].role == factory.entries[required].role) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    status = PALETTE_STATUS_INCOMPLETE_ROLE_SET;
                    break;
                }
            }
        }
    }

    if (status == PALETTE_STATUS_SUCCESS) {
        ESP_LOGI(TAG,
                 "CP palette save request: mode=%u count=%u",
                 mode,
                 count);

        status = map_palette_result(
            clock_palette_save_mode_override(mode, entries, count));

        if (status == PALETTE_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "CP palette save ok: mode=%u", mode);
        }
    }

    if (status != PALETTE_STATUS_SUCCESS) {
        ESP_LOGW(TAG,
                 "CP palette save rejected: mode=%u status=%02X",
                 mode,
                 status);
    }

    return build_palette_ack(tx,
                             tx_max,
                             board_id,
                             'c',
                             'p',
                             mode,
                             status);
}

static int handle_dp_command(const uint8_t *p,
                             int len,
                             uint8_t *tx,
                             int tx_max)
{
    uint8_t board_id = p[3];
    uint8_t mode = 0;
    palette_protocol_status_t status = PALETTE_STATUS_SUCCESS;

    if (board_id != 0x00 || board_id == 0x5C) {
        ESP_LOGW(TAG, "Ignoring DP command for invalid board ID: %u", board_id);
        return -1;
    }

    if (len < 8) {
        status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
    } else if (!decode_ascii_hex_u8(&p[6], &mode)) {
        status = PALETTE_STATUS_INVALID_HEX;
    } else if (!clock_palette_is_supported_mode(mode)) {
        status = PALETTE_STATUS_UNSUPPORTED_MODE;
    } else if (len != 9 || p[8] != '\\') {
        status = PALETTE_STATUS_INVALID_COUNT_OR_LENGTH;
    } else {
        status = map_palette_result(
            clock_palette_restore_mode_defaults(mode));
    }

    if (status == PALETTE_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "DP palette defaults restored: mode=%u", mode);
    } else {
        ESP_LOGW(TAG,
                 "DP palette restore rejected: mode=%u status=%02X",
                 mode,
                 status);
    }

    return build_palette_ack(tx,
                             tx_max,
                             board_id,
                             'd',
                             'p',
                             mode,
                             status);
}

static int handle_palette_command(const uint8_t *p,
                                  int len,
                                  uint8_t *tx,
                                  int tx_max)
{
    if (p[4] == 'L' && p[5] == 'P') {
        return handle_lp_command(p, len, tx, tx_max);
    }

    if (p[4] == 'C' && p[5] == 'P') {
        return handle_cp_command(p, len, tx, tx_max);
    }

    return handle_dp_command(p, len, tx, tx_max);
}















int clock_protocol_rx_callback(const uint8_t *p,
                               int len,
                               uint8_t *tx,
                               int tx_max)
{
	ESP_LOGI(TAG, "Command from Ethernet received: %d bytes", len);

	    /*
	     * Ignore keep-alive / poll.
	     */
	    if (len == 3 &&
	        p[0] == 0x00 &&
	        p[1] == 0x00 &&
	        p[2] == 0x00) {
	        ESP_LOGI(TAG, "Ethernet keep-alive / poll received");
	        return -1;
	    }

	    if (is_palette_command(p, len)) {
	        return handle_palette_command(p, len, tx, tx_max);
	    }

	    if (is_set_mode_command(p, len)) {
	        return handle_set_mode_command(p, len, tx, tx_max);
	    }

	    /*
	     * CT command:
	     * /TA <ID> CT <Formato2412> <Intensidad_Luminosa> \
	     *
		 * Formato2412 observed from Zeit software:
		 * 0 = 12H
		 * 1 = 24H
		 *
		 * Note:
		 * This is opposite to what the document appears to say,
		 * but this mapping matches the actual PC software behavior.
	     *
	     * Intensidad_Luminosa:
	     * 0-255 from software.
	     */
	    if (len == 9 &&
	        p[0] == '/' &&
	        p[1] == 'T' &&
	        p[2] == 'A' &&
	        p[4] == 'C' &&
	        p[5] == 'T' &&
	        p[8] == '\\') {

	        uint8_t board_id = p[3];
	        uint8_t format_2412 = p[6];
	        uint8_t intensity = p[7];

	        ESP_LOGI(TAG,
	                 "CT command: id=%u format=%u intensity=%u",
	                 board_id,
	                 format_2412,
	                 intensity);

	        /*
	         * Accept only board ID 0 for now.
	         */
	        if (board_id != 0x00) {
	            ESP_LOGW(TAG, "Ignoring CT command for different board ID: %u", board_id);
	            return -1;
	        }

			if (format_2412 > 1) {
			    ESP_LOGW(TAG, "Invalid CT format value: %u", format_2412);
			    return -1;
			}

			hour_format_t new_format =
			    (format_2412 == 0) ? FORMAT_12H : FORMAT_24H;

	        int new_brightness_level =
	            ethernet_intensity_to_level(intensity);

	        ESP_LOGI(TAG,
	                 "Ethernet intensity %u converted to brightness level %d",
	                 intensity,
	                 new_brightness_level);

			 portENTER_CRITICAL(s_ctx.data_mux);

			 *s_ctx.eth_brightness_level = new_brightness_level;
			 *s_ctx.eth_brightness_pending = true;

			 *s_ctx.eth_format = new_format;
			 *s_ctx.eth_format_pending = true;

			 portEXIT_CRITICAL(s_ctx.data_mux);

	        return 0;
	    }
		
		/*
		 * ES command:
		 * /TA <ID> ES \
		 *
		 * Estado del Sistema / Is Online.
		 * ACK is generated by clock_ethernet.cpp:
		 * /ta <ID> es \
		 */
		 if (len == 7 &&
		     p[0] == '/' &&
		     p[1] == 'T' &&
		     p[2] == 'A' &&
		     p[4] == 'E' &&
		     p[5] == 'S' &&
		     p[6] == '\\') {

		     uint8_t board_id = p[3];

		     ESP_LOGI(TAG, "ES online/status command received: id=%u", board_id);

		     if (board_id != 0x00) {
		         ESP_LOGW(TAG, "Ignoring ES command for different board ID: %u", board_id);
		         return -1;
		     }

			 clock_alarm_arm_full_replacement();

		     return 0;
		 }
		
		/*
		 * UC command:
		 * /TA <ID> UC <ss><mm><hh><DoW><dd><MM><yy> \
		 *
		 * Time bytes are BCD according to the protocol.
		 */
		if (len == 14 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'U' &&
		    p[5] == 'C' &&
		    p[13] == '\\') {

		    uint8_t board_id = p[3];

		    if (board_id != 0x00) {
		        ESP_LOGW(TAG, "Ignoring UC command for different board ID: %u", board_id);
		        return -1;
		    }

		    for (int i = 6; i <= 12; i++) {
		        if (!bcd_is_valid(p[i])) {
		            ESP_LOGW(TAG, "Invalid BCD byte in UC at index %d: 0x%02X", i, p[i]);
		            return -1;
		        }
		    }

		    ds3231_time_t new_time = {};

			new_time.second = bcd_to_dec(p[6]);
			new_time.minute = bcd_to_dec(p[7]);
			new_time.hour   = bcd_to_dec(p[8]);

			uint8_t received_dow = bcd_to_dec(p[9]);

			new_time.day   = bcd_to_dec(p[10]);
			new_time.month = bcd_to_dec(p[11]);
			new_time.year  = 2000 + bcd_to_dec(p[12]);

			/*
			 * Do not trust Ethernet DoW because the PC software may use
			 * a different convention:
			 *
			 * 0 = Sunday, 1 = Monday
			 * or
			 * 1 = Sunday, 2 = Monday
			 *
			 * Our clock already has a known-good weekday calculator.
			 */
			new_time.day_of_week = clock_menu_calculate_weekday(
			    new_time.day,
			    new_time.month,
			    new_time.year
			);

			ESP_LOGI(TAG,
			         "UC received DoW=%u, calculated DoW=%d",
			         received_dow,
			         new_time.day_of_week);

		    if (!rtc_time_is_valid(&new_time)) {
		        ESP_LOGW(TAG,
		                 "Invalid UC time: %04d-%02d-%02d %02d:%02d:%02d DOW=%d",
		                 new_time.year,
		                 new_time.month,
		                 new_time.day,
		                 new_time.hour,
		                 new_time.minute,
		                 new_time.second,
		                 new_time.day_of_week);
		        return -1;
		    }

			portENTER_CRITICAL(s_ctx.data_mux);

			*s_ctx.eth_time = new_time;
			*s_ctx.eth_time_pending = true;

			portEXIT_CRITICAL(s_ctx.data_mux);

		    ESP_LOGI(TAG,
		             "UC time received: %04d-%02d-%02d %02d:%02d:%02d DOW=%d",
		             new_time.year,
		             new_time.month,
		             new_time.day,
		             new_time.hour,
		             new_time.minute,
		             new_time.second,
		             new_time.day_of_week);

		    return 0;
		}
		
		/*
		 * RC command:
		 * /TA <ID> RC \
		 *
		 * Response:
		 * /ta <ID> rc <IMW><Formato2412><Intensidad><ss><mm><hh><DoW><dd><MM><yy> \
		 */
		if (len == 7 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'R' &&
		    p[5] == 'C' &&
		    p[6] == '\\') {

		    uint8_t board_id = p[3];

		    ESP_LOGI(TAG, "RC read config command received: id=%u", board_id);

		    if (board_id != 0x00) {
		        ESP_LOGW(TAG, "Ignoring RC command for different board ID: %u", board_id);
		        return -1;
		    }

		    if (tx == NULL || tx_max < 17) {
		        ESP_LOGE(TAG, "TX buffer too small for RC response");
		        return -1;
		    }

		    ds3231_time_t now_copy;
		    hour_format_t format_copy;
		    int brightness_level_copy;
		    bool rtc_valid_copy;

			portENTER_CRITICAL(s_ctx.data_mux);

			now_copy = *s_ctx.now;
			format_copy = *s_ctx.clock_format;
			brightness_level_copy = *s_ctx.brightness_level;
			rtc_valid_copy = *s_ctx.rtc_valid;

			portEXIT_CRITICAL(s_ctx.data_mux);

		    if (!rtc_valid_copy) {
		        ESP_LOGW(TAG, "RC requested but RTC is not valid");
		        return -1;
		    }

		    uint8_t format_2412 = (format_copy == FORMAT_12H) ? 0 : 1;
		    uint8_t intensity = brightness_level_to_protocol_intensity(brightness_level_copy);

		    /*
		     * RC document says time fields are HEX, not BCD.
		     */
		    tx[0]  = '/';
		    tx[1]  = 't';
		    tx[2]  = 'a';
		    tx[3]  = board_id;
		    tx[4]  = 'r';
		    tx[5]  = 'c';

		    tx[6]  = 0x01; // IMW: IsMemoryWritten. Use 1 = valid/configured.
		    tx[7]  = format_2412;
		    tx[8]  = intensity;

		    tx[9]  = (uint8_t)now_copy.second;
		    tx[10] = (uint8_t)now_copy.minute;
		    tx[11] = (uint8_t)now_copy.hour;

		    /*
		     * Use your internal day_of_week value.
		     * If software shows wrong weekday later, we can map it here.
		     */
		    tx[12] = (uint8_t)now_copy.day_of_week;

		    tx[13] = (uint8_t)now_copy.day;
		    tx[14] = (uint8_t)now_copy.month;
		    tx[15] = (uint8_t)(now_copy.year % 100);

		    tx[16] = '\\';

		    ESP_LOGI(TAG,
		             "RC response: format=%u intensity=%u time=%04d-%02d-%02d %02d:%02d:%02d DOW=%d",
		             format_2412,
		             intensity,
		             now_copy.year,
		             now_copy.month,
		             now_copy.day,
		             now_copy.hour,
		             now_copy.minute,
		             now_copy.second,
		             now_copy.day_of_week);

		    return 17;
		}
		
		/*
		 * CA command:
		 * /TA <ID> CA <Alarma_ID><Hora_Alarma[2]><Frecuencia><Duracion&Efecto> \
		 *
		 * ACK:
		 * /ta <ID> ca <Alarma_ID> \
		 */
		if (len == 12 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'C' &&
		    p[5] == 'A' &&
		    p[11] == '\\') {

		    uint8_t board_id = p[3];

		    if (board_id != 0x00) {
		        ESP_LOGW(TAG, "Ignoring CA command for different board ID: %u", board_id);
		        return -1;
		    }

		    uint8_t alarm_id = p[6];
		    uint8_t alarm_hh = p[7];
		    uint8_t alarm_mm = p[8];
		    uint8_t frequency = p[9];
		    uint8_t duration_effect = p[10];

		    if (alarm_id < 1 || alarm_id > MAX_ETH_ALARMS) {
		        ESP_LOGW(TAG, "Invalid CA alarm_id=%u", alarm_id);
		        return -1;
		    }

		    if (alarm_hh > 23 || alarm_mm > 59) {
		        ESP_LOGW(TAG, "Invalid CA alarm time: %02u:%02u", alarm_hh, alarm_mm);
		        return -1;
		    }

			esp_err_t alarm_ret = clock_alarm_store_from_ca(alarm_id,
			                                                alarm_hh,
			                                                alarm_mm,
			                                                frequency,
			                                                duration_effect);

			if (alarm_ret != ESP_OK) {
			    return -1;
			}


		    tx[0] = '/';
		    tx[1] = 't';
		    tx[2] = 'a';
		    tx[3] = board_id;
		    tx[4] = 'c';
		    tx[5] = 'a';
		    tx[6] = alarm_id;
		    tx[7] = '\\';

		    return 8;
		}

		/*
		 * DA command:
		 * /TA <ID> DA <Alarma_ID> \
		 *
		 * ACK:
		 * /ta <ID> da <Alarma_ID> \
		 */
		if (len == 8 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'D' &&
		    p[5] == 'A' &&
		    p[7] == '\\') {

		    uint8_t board_id = p[3];

		    if (board_id != 0x00) {
		        ESP_LOGW(TAG, "Ignoring DA command for different board ID: %u", board_id);
		        return -1;
		    }

		    uint8_t alarm_id = p[6];

		    if (alarm_id < 1 || alarm_id > MAX_ETH_ALARMS) {
		        ESP_LOGW(TAG, "Invalid DA alarm_id=%u", alarm_id);
		        return -1;
		    }

		    if (tx == NULL || tx_max < 8) {
		        ESP_LOGE(TAG, "TX buffer too small for DA ACK");
		        return -1;
		    }

		    ESP_LOGI(TAG, "DA delete alarm: id=%u", alarm_id);

		    esp_err_t alarm_ret = clock_alarm_delete_from_da(alarm_id);

		    if (alarm_ret != ESP_OK) {
		        return -1;
		    }

		    tx[0] = '/';
		    tx[1] = 't';
		    tx[2] = 'a';
		    tx[3] = board_id;
		    tx[4] = 'd';
		    tx[5] = 'a';
		    tx[6] = alarm_id;
		    tx[7] = '\\';

		    return 8;
		}
		
		/*
		 * LA command:
		 * /TA <ID> LA <Alarma_ID> \
		 *
		 * ACK:
		 * /ta <ID> la <Alarma_ID><HH><MM><Frecuencia><Duracion&Efecto> \
		 */
		if (len == 8 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'L' &&
		    p[5] == 'A' &&
		    p[7] == '\\') {

		    uint8_t board_id = p[3];

		    if (board_id != 0x00) {
		        ESP_LOGW(TAG, "Ignoring LA command for different board ID: %u", board_id);
		        return -1;
		    }

		    uint8_t alarm_id = p[6];

		    if (alarm_id < 1 || alarm_id > MAX_ETH_ALARMS) {
		        ESP_LOGW(TAG, "Invalid LA alarm_id=%u", alarm_id);
		        return -1;
		    }

			ethernet_alarm_t alarm_copy;

			if (!clock_alarm_read(alarm_id, &alarm_copy)) {
			    ESP_LOGW(TAG, "Invalid LA alarm_id=%u", alarm_id);
			    return -1;
			}

		    bool alarm_enabled = (alarm_copy.frequency & 0x80) != 0;

		    ESP_LOGI(TAG,
		             "LA read alarm: id=%u configured=%d enabled=%d time=%02u:%02u freq=0x%02X dur_eff=0x%02X",
		             alarm_id,
		             alarm_copy.configured,
		             alarm_enabled,
		             alarm_copy.time_hh,
		             alarm_copy.time_mm,
		             alarm_copy.frequency,
		             alarm_copy.duration_effect);

		    if (tx == NULL || tx_max < 12) {
		        return -1;
		    }

		    tx[0] = '/';
		    tx[1] = 't';
		    tx[2] = 'a';
		    tx[3] = board_id;
		    tx[4] = 'l';
		    tx[5] = 'a';

		    tx[6]  = alarm_id;
		    tx[7]  = alarm_copy.time_hh;
		    tx[8]  = alarm_copy.time_mm;
		    tx[9]  = alarm_copy.frequency;
		    tx[10] = alarm_copy.duration_effect;

		    tx[11] = '\\';

		    return 12;
		}
		
		
		
		
		
		/*
		 * DL command:
		 * /TA <ID> DL \
		 *
		 * Permanently removes /sdcard/logo.lgo and activates
		 * the logo compiled into the ESP32 firmware.
		 *
		 * Response:
		 * /ta <ID> dl <Result> \
		 *
		 * Result:
		 * 0x00 = success
		 * 0x01 = logo operation busy
		 * 0x02 = SD-card/file operation failed
		 */
		if (len == 7 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'D' &&
		    p[5] == 'L' &&
		    p[6] == '\\') {

		    const uint8_t board_id = p[3];

		    ESP_LOGI(
		        TAG,
		        "DL restore default logo command received: board_id=%u",
		        board_id
		    );

		    /*
		     * This firmware currently accepts only Board ID 0.
		     * Board ID 0x5C is also invalid because it is the delimiter.
		     */
		    if (board_id != 0x00 || board_id == 0x5C) {
		        ESP_LOGW(
		            TAG,
		            "Ignoring DL command for different or invalid board ID: %u",
		            board_id
		        );

		        return -1;
		    }

		    if (tx == nullptr || tx_max < 8) {
		        ESP_LOGE(
		            TAG,
		            "TX buffer too small for DL response"
		        );

		        return -1;
		    }

		    const clock_logo_restore_result_t restore_result =
		        clock_logo_restore_compiled_default();

		    uint8_t protocol_result = 0x02;

		    switch (restore_result) {
		        case CLOCK_LOGO_RESTORE_OK:
		            protocol_result = 0x00;

		            /*
		             * Force the display-mode logic to fetch the newly
		             * activated compiled logo.
		             */
		            clock_modes_reset_sequences();
		            break;

		        case CLOCK_LOGO_RESTORE_BUSY:
		            protocol_result = 0x01;
		            break;

		        case CLOCK_LOGO_RESTORE_STORAGE_ERROR:
		        default:
		            protocol_result = 0x02;
		            break;
		    }

		    tx[0] = '/';
		    tx[1] = 't';
		    tx[2] = 'a';
		    tx[3] = board_id;
		    tx[4] = 'd';
		    tx[5] = 'l';
		    tx[6] = protocol_result;
		    tx[7] = '\\';

		    ESP_LOGI(
		        TAG,
		        "DL response: board_id=%u result=0x%02X",
		        board_id,
		        protocol_result
		    );

		    /*
		     * clock_ethernet.cpp sends exactly 8 bytes from tx.
		     */
		    return 8;
		}
		
		
		
		
		
		
		
		
		
		/*
		 * RT command:
		 * /TA <ID> RT <Reset_ID> \
		 *
		 * reset_id = 0x00 -> reset device settings/defaults
		 *
		 * ACK:
		 * /ta <ID> rt \
		 */
		 /*
		  * RT command:
		  * /TA <ID> RT <Reset_ID> \
		  *
		  * reset_id = 0x00 -> reset all device settings/defaults
		  *
		  * The firmware also permanently removes the uploaded SD logo
		  * and activates the compiled default logo.
		  *
		  * ACK:
		  * /ta <ID> rt \
		  */
		 if (len == 8 &&
		     p[0] == '/' &&
		     p[1] == 'T' &&
		     p[2] == 'A' &&
		     p[4] == 'R' &&
		     p[5] == 'T' &&
		     p[7] == '\\') {

		     const uint8_t board_id = p[3];
		     const uint8_t reset_id = p[6];

		     ESP_LOGI(
		         TAG,
		         "RT reset command received: board_id=%u reset_id=0x%02X",
		         board_id,
		         reset_id
		     );

		     if (board_id != 0x00 || board_id == 0x5C) {
		         ESP_LOGW(
		             TAG,
		             "Ignoring RT command for different or invalid board ID: %u",
		             board_id
		         );

		         return -1;
		     }

		     if (reset_id != 0x00) {
		         ESP_LOGW(
		             TAG,
		             "Unknown RT reset_id=0x%02X",
		             reset_id
		         );

		         return -1;
		     }

		     /*
		      * Restore the compiled logo synchronously before reporting
		      * successful acceptance of the factory reset.
		      */
		     const clock_logo_restore_result_t logo_result =
		         clock_logo_restore_compiled_default();

		     if (logo_result == CLOCK_LOGO_RESTORE_BUSY) {
		         ESP_LOGW(
		             TAG,
		             "RT factory reset rejected: logo operation busy"
		         );

		         /*
		          * Do not send a successful RT ACK.
		          */
		         return -1;
		     }

		     if (logo_result != CLOCK_LOGO_RESTORE_OK) {
		         ESP_LOGE(
		             TAG,
		             "RT factory reset failed restoring compiled logo: result=%d",
		             static_cast<int>(logo_result)
		         );

		         /*
		          * Do not send a successful RT ACK when the persistent
		          * SD logo could not be removed.
		          */
		         return -1;
		     }

		     clock_modes_reset_sequences();

		     /*
		      * Let the main task restore brightness, format, mode,
		      * alarms, and the other existing defaults.
		      */
		     portENTER_CRITICAL(s_ctx.data_mux);
		     *s_ctx.factory_reset_pending = true;
		     portEXIT_CRITICAL(s_ctx.data_mux);

		     ESP_LOGW(
		         TAG,
		         "RT Reset All received: compiled logo restored, "
		         "factory reset pending"
		     );

		     /*
		      * Generic ACK will be sent by clock_ethernet.cpp:
		      * /ta <ID> rt \
		      */
		     return 0;
		 }
		
		/*
		 * HB heartbeat command:
		 *
		 * Request:
		 * /TA <ID> HB <Sequence_Hex_High><Sequence_Hex_Low> \
		 *
		 * Response:
		 * /ta <ID> hb <Sequence_Hex_High><Sequence_Hex_Low> \
		 *
		 * Example:
		 * Request:  2F 54 41 00 48 42 30 35 5C
		 * Response: 2F 74 61 00 68 62 30 35 5C
		 */
		if (len == 9 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'H' &&
		    p[5] == 'B' &&
		    p[8] == '\\') {

		    const uint8_t board_id = p[3];

		    if (board_id != 0x00) {
		        ESP_LOGW(
		            TAG,
		            "Ignoring HB command for different board ID: %u",
		            board_id
		        );

		        return -1;
		    }

		    if (!is_ascii_hex(p[6]) ||
		        !is_ascii_hex(p[7])) {

		        ESP_LOGW(
		            TAG,
		            "Invalid HB sequence: 0x%02X 0x%02X",
		            p[6],
		            p[7]
		        );

		        return -1;
		    }

		    if (tx == NULL || tx_max < 9) {
		        ESP_LOGE(TAG, "TX buffer too small for HB response");
		        return -1;
		    }

		    /*
		     * Build matching heartbeat ACK.
		     */
		    tx[0] = '/';
		    tx[1] = 't';
		    tx[2] = 'a';
		    tx[3] = board_id;
		    tx[4] = 'h';
		    tx[5] = 'b';
		    tx[6] = p[6];
		    tx[7] = p[7];
		    tx[8] = '\\';

		    ESP_LOGI(
		        TAG,
		        "Heartbeat received: board=%u sequence=%c%c",
		        board_id,
		        static_cast<char>(p[6]),
		        static_cast<char>(p[7])
		    );

		    /*
		     * clock_ethernet.cpp sends this number of bytes from tx.
		     */
		    return 9;
		}
		
		
		if (len == 7 &&
		    p[0] == '/' &&
		    p[1] == 'T' &&
		    p[2] == 'A' &&
		    p[4] == 'N' &&
		    p[5] == 'M' &&
		    p[6] == '\\') {

		    const uint8_t board_id = p[3];

		    if (board_id != 0x00) {
		        ESP_LOGW(
		            TAG,
		            "Ignoring NM command for different board ID: %u",
		            board_id
		        );
		        return -1;
		    }

		    if (tx == nullptr || tx_max < 8) {
		        ESP_LOGE(TAG, "TX buffer too small for NM response");
		        return -1;
		    }

			const uint8_t new_mode = clock_modes_advance_mode();

			tx[0] = '/';
			tx[1] = 't';
			tx[2] = 'a';
			tx[3] = board_id;
			tx[4] = 'n';
			tx[5] = 'm';
			tx[6] = new_mode;
			tx[7] = '\\';

			return 8;
		}				

		ESP_LOGW(TAG, "Unknown Ethernet command");
		return -1;
}
