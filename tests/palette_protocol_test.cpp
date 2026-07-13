#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "clock_palette.h"
#include "clock_protocol.h"
#include "clock_protocol_stream.h"
#include "esp_err.h"

void palette_test_reset_nvs(void);
void palette_test_reset_counters(void);
void palette_test_set_commit_result(esp_err_t result);
int palette_test_set_count(void);
int palette_test_commit_count(void);
void protocol_test_reset_alarms(void);
void protocol_test_reset_modes(void);
uint8_t protocol_test_display_mode(void);
int protocol_test_mode_save_count(void);
int protocol_test_mode_reset_count(void);
int protocol_test_mode_advance_count(void);
void protocol_test_set_mode_result(esp_err_t result);
void protocol_test_force_display_mode(uint8_t mode);

static int s_test_count = 0;
static int s_brightness_level = 5;
static bool s_eth_brightness_pending = false;
static int s_eth_brightness_level = 5;
static bool s_eth_format_pending = false;
static hour_format_t s_eth_format = FORMAT_12H;
static bool s_eth_time_pending = false;
static ds3231_time_t s_eth_time = {};
static bool s_factory_reset_pending = false;
static ds3231_time_t s_now = {30, 20, 10, 2, 13, 7, 2026};
static bool s_rtc_valid = true;
static hour_format_t s_clock_format = FORMAT_12H;
static portMUX_TYPE s_data_mux = portMUX_INITIALIZER_UNLOCKED;

static void fail_at(int line, const char *expr)
{
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
    std::exit(1);
}

#define REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            fail_at(__LINE__, #expr); \
        } \
    } while (0)

#define REQUIRE_EQ(actual, expected) REQUIRE((actual) == (expected))

static int hex_nibble(uint8_t value)
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

static uint8_t decode_hex(const uint8_t *data)
{
    int high = hex_nibble(data[0]);
    int low = hex_nibble(data[1]);
    REQUIRE(high >= 0);
    REQUIRE(low >= 0);
    return (uint8_t)((high << 4) | low);
}

static void append_hex(std::vector<uint8_t> *bytes, uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    bytes->push_back((uint8_t)digits[value >> 4]);
    bytes->push_back((uint8_t)digits[value & 0x0f]);
}

static std::vector<uint8_t> make_request_prefix(char command_1,
                                                 char command_2)
{
    return {'/', 'T', 'A', 0x00,
            (uint8_t)command_1, (uint8_t)command_2};
}

static std::vector<uint8_t> make_lp(uint8_t mode)
{
    std::vector<uint8_t> request = make_request_prefix('L', 'P');
    append_hex(&request, mode);
    request.push_back('\\');
    return request;
}

static std::vector<uint8_t> make_dp(uint8_t mode)
{
    std::vector<uint8_t> request = make_request_prefix('D', 'P');
    append_hex(&request, mode);
    request.push_back('\\');
    return request;
}

static std::vector<uint8_t> make_sm(uint8_t mode)
{
    std::vector<uint8_t> request = make_request_prefix('S', 'M');
    append_hex(&request, mode);
    request.push_back('\\');
    return request;
}

static std::vector<uint8_t> make_rm(void)
{
    std::vector<uint8_t> request = make_request_prefix('R', 'M');
    request.push_back('\\');
    return request;
}

static clock_mode_palette_t factory_palette(uint8_t mode)
{
    clock_mode_palette_t palette = {};
    REQUIRE(clock_palette_get_factory_snapshot(mode, &palette));
    return palette;
}

static void set_color(clock_mode_palette_t *palette,
                      uint8_t role,
                      clock_rgb_t color)
{
    for (uint8_t i = 0; i < palette->count; ++i) {
        if (palette->entries[i].role == role) {
            palette->entries[i].color = color;
            return;
        }
    }

    REQUIRE(false);
}

static std::vector<uint8_t> make_cp(
    uint8_t mode,
    uint8_t version,
    const clock_palette_entry_t *entries,
    uint8_t count)
{
    std::vector<uint8_t> request = make_request_prefix('C', 'P');
    append_hex(&request, mode);
    append_hex(&request, version);
    append_hex(&request, count);

    for (uint8_t i = 0; i < count; ++i) {
        append_hex(&request, entries[i].role);
        append_hex(&request, entries[i].color.r);
        append_hex(&request, entries[i].color.g);
        append_hex(&request, entries[i].color.b);
    }

    request.push_back('\\');
    return request;
}

static std::vector<uint8_t> make_cp(uint8_t mode,
                                    const clock_mode_palette_t &palette)
{
    return make_cp(mode,
                   CLOCK_PALETTE_SCHEMA_VERSION,
                   palette.entries,
                   palette.count);
}

static std::vector<uint8_t> dispatch(const std::vector<uint8_t> &request,
                                     int *out_result = nullptr)
{
    uint8_t tx[256] = {};
    int result = clock_protocol_rx_callback(request.data(),
                                            (int)request.size(),
                                            tx,
                                            sizeof(tx));

    if (out_result != nullptr) {
        *out_result = result;
    }

    if (result <= 0) {
        return {};
    }

    return std::vector<uint8_t>(tx, tx + result);
}

static uint8_t response_status(const std::vector<uint8_t> &response)
{
    REQUIRE(response.size() >= 10);
    return decode_hex(&response[8]);
}

static clock_rgb_t palette_color(uint8_t mode, uint8_t role)
{
    clock_rgb_t color = {};
    REQUIRE(clock_palette_get_color(mode, role, &color));
    return color;
}

static bool rgb_equal(clock_rgb_t color,
                      uint8_t r,
                      uint8_t g,
                      uint8_t b)
{
    return color.r == r && color.g == g && color.b == b;
}

static void reset_state(void)
{
    palette_test_reset_nvs();
    REQUIRE_EQ(clock_palette_init(), ESP_OK);
    palette_test_reset_counters();
    palette_test_set_commit_result(ESP_OK);
    protocol_test_reset_alarms();
    protocol_test_reset_modes();

    clock_protocol_context_t context = {
        .brightness_level = &s_brightness_level,
        .eth_brightness_pending = &s_eth_brightness_pending,
        .eth_brightness_level = &s_eth_brightness_level,
        .eth_format_pending = &s_eth_format_pending,
        .eth_format = &s_eth_format,
        .eth_time_pending = &s_eth_time_pending,
        .eth_time = &s_eth_time,
        .factory_reset_pending = &s_factory_reset_pending,
        .now = &s_now,
        .rtc_valid = &s_rtc_valid,
        .clock_format = &s_clock_format,
        .data_mux = &s_data_mux,
    };

    clock_protocol_init(&context);
}

static void require_palette_response_prefix(
    const std::vector<uint8_t> &response,
    char command_1,
    char command_2)
{
    REQUIRE(response.size() >= 11);
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], 0x00);
    REQUIRE_EQ(response[4], (uint8_t)command_1);
    REQUIRE_EQ(response[5], (uint8_t)command_2);
    REQUIRE_EQ(response.back(), '\\');
}

static void test_lp_supported_modes_and_complete_sorted_ascii_response(void)
{
    reset_state();

    for (uint8_t mode = 1; mode <= 3; ++mode) {
        std::vector<uint8_t> response = dispatch(make_lp(mode));
        require_palette_response_prefix(response, 'l', 'p');
        REQUIRE_EQ(response_status(response), 0x00);
        REQUIRE_EQ(decode_hex(&response[6]), mode);
        REQUIRE_EQ(decode_hex(&response[10]), CLOCK_PALETTE_SCHEMA_VERSION);

        uint8_t count = decode_hex(&response[12]);
        REQUIRE_EQ(count, mode == 3 ? 7 : 6);
        REQUIRE_EQ(response.size(), 15u + ((size_t)count * 8));

        uint8_t previous_role = 0;

        for (uint8_t i = 0; i < count; ++i) {
            size_t offset = 14 + ((size_t)i * 8);
            uint8_t role = decode_hex(&response[offset]);
            REQUIRE(i == 0 || role > previous_role);
            previous_role = role;

            for (size_t field = offset; field < offset + 8; ++field) {
                REQUIRE(hex_nibble(response[field]) >= 0);
            }
        }
    }
}

static void test_lp_mode_4_error_shape(void)
{
    reset_state();
    std::vector<uint8_t> response = dispatch(make_lp(4));
    require_palette_response_prefix(response, 'l', 'p');
    REQUIRE_EQ(response.size(), 15u);
    REQUIRE_EQ(response_status(response), 0x01);
    REQUIRE_EQ(decode_hex(&response[10]), 0);
    REQUIRE_EQ(decode_hex(&response[12]), 0);
}

static void test_cp_supported_modes_and_readback(void)
{
    reset_state();

    for (uint8_t mode = 1; mode <= 3; ++mode) {
        clock_mode_palette_t palette = factory_palette(mode);
        set_color(&palette,
                  CLOCK_PALETTE_ROLE_TIME,
                  {(uint8_t)mode, 0x22, 0x33});
        std::vector<uint8_t> response = dispatch(make_cp(mode, palette));
        require_palette_response_prefix(response, 'c', 'p');
        REQUIRE_EQ(response_status(response), 0x00);
        REQUIRE(rgb_equal(palette_color(mode, CLOCK_PALETTE_ROLE_TIME),
                          mode,
                          0x22,
                          0x33));

        std::vector<uint8_t> lp_response = dispatch(make_lp(mode));
        REQUIRE_EQ(response_status(lp_response), 0x00);
        std::string text(lp_response.begin(), lp_response.end());
        char expected[9];
        std::snprintf(expected, sizeof(expected), "01%02X2233", mode);
        REQUIRE(text.find(expected) != std::string::npos);
    }
}

static void test_cp_validation_statuses(void)
{
    reset_state();
    clock_mode_palette_t palette = factory_palette(1);

    std::vector<uint8_t> request = make_cp(4, palette);
    REQUIRE_EQ(response_status(dispatch(request)), 0x01);

    request = make_cp(1, 2, palette.entries, palette.count);
    REQUIRE_EQ(response_status(dispatch(request)), 0x02);

    request = make_cp(1, palette);
    request.erase(request.end() - 2);
    REQUIRE_EQ(response_status(dispatch(request)), 0x03);

    request = make_cp(1, palette);
    request[10] = '0';
    request[11] = '0';
    REQUIRE_EQ(response_status(dispatch(request)), 0x03);

    request = make_cp(1, palette);
    request[20] = request[12];
    request[21] = request[13];
    REQUIRE_EQ(response_status(dispatch(request)), 0x04);

    request = make_cp(1, palette);
    request[20] = '0';
    request[21] = '3';
    REQUIRE_EQ(response_status(dispatch(request)), 0x05);

    request = make_cp(1,
                      CLOCK_PALETTE_SCHEMA_VERSION,
                      palette.entries,
                      palette.count - 1);
    REQUIRE_EQ(response_status(dispatch(request)), 0x06);

    request = make_cp(1, palette);
    request[15] = 'G';
    REQUIRE_EQ(response_status(dispatch(request)), 0x07);
}

static void test_cp_nvs_failure_and_unchanged_skip(void)
{
    reset_state();
    clock_mode_palette_t palette = factory_palette(1);

    std::vector<uint8_t> response = dispatch(make_cp(1, palette));
    REQUIRE_EQ(response_status(response), 0x00);
    REQUIRE_EQ(palette_test_set_count(), 0);
    REQUIRE_EQ(palette_test_commit_count(), 0);

    set_color(&palette, CLOCK_PALETTE_ROLE_TIME, {1, 2, 3});
    palette_test_set_commit_result(ESP_FAIL);
    response = dispatch(make_cp(1, palette));
    REQUIRE_EQ(response_status(response), 0x08);
    REQUIRE(rgb_equal(palette_color(1, CLOCK_PALETTE_ROLE_TIME),
                      255,
                      255,
                      255));
    palette_test_set_commit_result(ESP_OK);
}

static void test_cp_ascii_005cff_delimiter_safety(void)
{
    reset_state();
    clock_mode_palette_t palette = factory_palette(1);
    set_color(&palette, CLOCK_PALETTE_ROLE_DATE, {0x00, 0x5c, 0xff});
    std::vector<uint8_t> request = make_cp(1, palette);

    size_t delimiter_count = 0;
    for (uint8_t byte : request) {
        if (byte == 0x5c) {
            delimiter_count++;
        }
    }
    REQUIRE_EQ(delimiter_count, 1u);
    REQUIRE_EQ(response_status(dispatch(request)), 0x00);

    std::vector<uint8_t> lp_response = dispatch(make_lp(1));
    REQUIRE_EQ(response_status(lp_response), 0x00);
    std::string text(lp_response.begin(), lp_response.end());
    REQUIRE(text.find("02005CFF") != std::string::npos);

    delimiter_count = 0;
    for (uint8_t byte : lp_response) {
        if (byte == 0x5c) {
            delimiter_count++;
        }
    }
    REQUIRE_EQ(delimiter_count, 1u);
}

static void test_dp_restores_only_selected_mode(void)
{
    reset_state();
    clock_mode_palette_t mode_1 = factory_palette(1);
    clock_mode_palette_t mode_2 = factory_palette(2);
    set_color(&mode_1, CLOCK_PALETTE_ROLE_TIME, {1, 1, 1});
    set_color(&mode_2, CLOCK_PALETTE_ROLE_TIME, {2, 2, 2});
    REQUIRE_EQ(response_status(dispatch(make_cp(1, mode_1))), 0x00);
    REQUIRE_EQ(response_status(dispatch(make_cp(2, mode_2))), 0x00);

    std::vector<uint8_t> response = dispatch(make_dp(1));
    require_palette_response_prefix(response, 'd', 'p');
    REQUIRE_EQ(response_status(response), 0x00);
    REQUIRE(rgb_equal(palette_color(1, CLOCK_PALETTE_ROLE_TIME),
                      255,
                      255,
                      255));
    REQUIRE(rgb_equal(palette_color(2, CLOCK_PALETTE_ROLE_TIME), 2, 2, 2));
    REQUIRE_EQ(response_status(dispatch(make_dp(4))), 0x01);

    std::vector<uint8_t> lp_response = dispatch(make_lp(1));
    std::string text(lp_response.begin(), lp_response.end());
    REQUIRE(text.find("01FFFFFF") != std::string::npos);
}

static void test_existing_rc_and_alarm_commands(void)
{
    reset_state();
    std::vector<uint8_t> rc = {'/', 'T', 'A', 0, 'R', 'C', '\\'};
    int rc_result = 0;
    std::vector<uint8_t> rc_response = dispatch(rc, &rc_result);
    REQUIRE_EQ(rc_result, 17);
    REQUIRE_EQ(rc_response[4], 'r');
    REQUIRE_EQ(rc_response[5], 'c');

    std::vector<uint8_t> ca = {
        '/', 'T', 'A', 0, 'C', 'A', 9, 7, 30, 0x88, 0x42, '\\'
    };
    std::vector<uint8_t> ca_response = dispatch(ca);
    REQUIRE_EQ(ca_response.size(), 8u);
    REQUIRE_EQ(ca_response[4], 'c');
    REQUIRE_EQ(ca_response[5], 'a');

    std::vector<uint8_t> la = {'/', 'T', 'A', 0, 'L', 'A', 9, '\\'};
    std::vector<uint8_t> la_response = dispatch(la);
    REQUIRE_EQ(la_response.size(), 12u);
    REQUIRE_EQ(la_response[7], 7);
    REQUIRE_EQ(la_response[8], 30);

    std::vector<uint8_t> da = {'/', 'T', 'A', 0, 'D', 'A', 9, '\\'};
    std::vector<uint8_t> da_response = dispatch(da);
    REQUIRE_EQ(da_response.size(), 8u);
    REQUIRE_EQ(da_response[4], 'd');
    REQUIRE_EQ(da_response[5], 'a');
}

static void require_sm_response(const std::vector<uint8_t> &response,
                                uint8_t mode,
                                uint8_t status)
{
    REQUIRE_EQ(response.size(), 11u);
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], 0x00);
    REQUIRE_EQ(response[4], 's');
    REQUIRE_EQ(response[5], 'm');
    REQUIRE_EQ(decode_hex(&response[6]), mode);
    REQUIRE_EQ(decode_hex(&response[8]), status);
    REQUIRE_EQ(response[10], '\\');
}

static void test_sm_supported_modes_update_and_save(void)
{
    reset_state();

    for (uint8_t mode = 1; mode <= 4; ++mode) {
        require_sm_response(dispatch(make_sm(mode)), mode, 0x00);
        REQUIRE_EQ(protocol_test_display_mode(), mode);
        REQUIRE_EQ(protocol_test_mode_save_count(), mode);
        REQUIRE_EQ(protocol_test_mode_reset_count(), mode);
    }
}

static void test_sm_validation_has_no_side_effects(void)
{
    reset_state();
    require_sm_response(dispatch(make_sm(5)), 5, 0x01);
    REQUIRE_EQ(protocol_test_display_mode(), 1);
    REQUIRE_EQ(protocol_test_mode_save_count(), 0);
    REQUIRE_EQ(protocol_test_mode_reset_count(), 0);

    std::vector<uint8_t> invalid_hex = make_sm(2);
    invalid_hex[6] = 'G';
    require_sm_response(dispatch(invalid_hex), 0, 0x03);
    REQUIRE_EQ(protocol_test_display_mode(), 1);
    REQUIRE_EQ(protocol_test_mode_save_count(), 0);

    std::vector<uint8_t> invalid_length = make_sm(3);
    invalid_length.insert(invalid_length.end() - 1, '0');
    require_sm_response(dispatch(invalid_length), 3, 0x02);
    REQUIRE_EQ(protocol_test_display_mode(), 1);
    REQUIRE_EQ(protocol_test_mode_save_count(), 0);
}

static void test_sm_apply_failures_are_reported(void)
{
    reset_state();
    protocol_test_set_mode_result(ESP_FAIL);
    require_sm_response(dispatch(make_sm(2)), 2, 0x04);
    REQUIRE_EQ(protocol_test_display_mode(), 2);
    REQUIRE_EQ(protocol_test_mode_save_count(), 1);

    reset_state();
    protocol_test_set_mode_result(ESP_ERR_INVALID_STATE);
    require_sm_response(dispatch(make_sm(3)), 3, 0x0A);
    REQUIRE_EQ(protocol_test_display_mode(), 3);
    REQUIRE_EQ(protocol_test_mode_save_count(), 1);
}

static void test_nm_behavior_and_response_are_unchanged(void)
{
    reset_state();
    require_sm_response(dispatch(make_sm(3)), 3, 0x00);

    std::vector<uint8_t> nm = {'/', 'T', 'A', 0, 'N', 'M', '\\'};
    std::vector<uint8_t> response = dispatch(nm);
    REQUIRE_EQ(response.size(), 8u);
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], 0x00);
    REQUIRE_EQ(response[4], 'n');
    REQUIRE_EQ(response[5], 'm');
    REQUIRE_EQ(response[6], 4);
    REQUIRE_EQ(response[7], '\\');
    REQUIRE_EQ(protocol_test_display_mode(), 4);
    REQUIRE_EQ(protocol_test_mode_advance_count(), 1);
}

static void require_rm_response(const std::vector<uint8_t> &response,
                                uint8_t mode,
                                uint8_t status)
{
    REQUIRE_EQ(response.size(), 11u);
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], 0x00);
    REQUIRE_EQ(response[4], 'r');
    REQUIRE_EQ(response[5], 'm');
    REQUIRE_EQ(decode_hex(&response[6]), mode);
    REQUIRE_EQ(decode_hex(&response[8]), status);
    REQUIRE_EQ(response[10], '\\');
}

static void test_rm_reports_modes_without_side_effects(void)
{
    reset_state();

    for (uint8_t mode = 1; mode <= 4; ++mode) {
        require_sm_response(dispatch(make_sm(mode)), mode, 0x00);
        const int save_count = protocol_test_mode_save_count();
        const int reset_count = protocol_test_mode_reset_count();
        const int advance_count = protocol_test_mode_advance_count();

        require_rm_response(dispatch(make_rm()), mode, 0x00);
        REQUIRE_EQ(protocol_test_display_mode(), mode);
        REQUIRE_EQ(protocol_test_mode_save_count(), save_count);
        REQUIRE_EQ(protocol_test_mode_reset_count(), reset_count);
        REQUIRE_EQ(protocol_test_mode_advance_count(), advance_count);
    }
}

static void test_rm_internal_failure_and_board_validation(void)
{
    reset_state();
    protocol_test_force_display_mode(5);
    require_rm_response(dispatch(make_rm()), 0, 0x0A);
    REQUIRE_EQ(protocol_test_display_mode(), 5);
    REQUIRE_EQ(protocol_test_mode_save_count(), 0);
    REQUIRE_EQ(protocol_test_mode_reset_count(), 0);

    std::vector<uint8_t> wrong_board = make_rm();
    wrong_board[3] = 1;
    int result = 0;
    REQUIRE(dispatch(wrong_board, &result).empty());
    REQUIRE_EQ(result, -1);
    REQUIRE_EQ(protocol_test_display_mode(), 5);
}

typedef struct {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<std::vector<uint8_t>> responses;
    std::vector<int> results;
} stream_capture_t;

static void capture_frame(const uint8_t *frame,
                          size_t length,
                          void *context)
{
    auto *capture = static_cast<stream_capture_t *>(context);
    capture->frames.emplace_back(frame, frame + length);
    int result = 0;
    capture->responses.push_back(dispatch(capture->frames.back(), &result));
    capture->results.push_back(result);
}

static void test_stream_fragmentation_and_partial_wait(void)
{
    reset_state();
    clock_protocol_stream_t stream = {};
    clock_protocol_stream_init(&stream);
    stream_capture_t capture;
    const uint8_t first[] = {'/', 'T', 'A'};
    clock_protocol_stream_result_t result =
        clock_protocol_stream_consume(&stream,
                                      first,
                                      sizeof(first),
                                      capture_frame,
                                      &capture);
    REQUIRE_EQ(result.frames_dispatched, 0u);
    REQUIRE(capture.frames.empty());

    const uint8_t second[] = {0, 'L', 'P', '0', '1', '\\'};
    result = clock_protocol_stream_consume(&stream,
                                           second,
                                           sizeof(second),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 1u);
    REQUIRE_EQ(capture.frames.size(), 1u);
    REQUIRE_EQ(response_status(capture.responses[0]), 0x00);
}

static void test_stream_coalesced_and_multiple_frames(void)
{
    reset_state();
    clock_protocol_stream_t stream = {};
    clock_protocol_stream_init(&stream);
    stream_capture_t capture;
    std::vector<uint8_t> rc = {'/', 'T', 'A', 0, 'R', 'C', '\\'};
    std::vector<uint8_t> lp_1 = make_lp(1);
    std::vector<uint8_t> lp_2 = make_lp(2);
    std::vector<uint8_t> combined = rc;
    combined.insert(combined.end(), lp_1.begin(), lp_1.end());
    combined.insert(combined.end(), lp_2.begin(), lp_2.end());

    clock_protocol_stream_result_t result =
        clock_protocol_stream_consume(&stream,
                                      combined.data(),
                                      combined.size(),
                                      capture_frame,
                                      &capture);
    REQUIRE_EQ(result.frames_dispatched, 3u);
    REQUIRE_EQ(capture.results[0], 17);
    REQUIRE_EQ(response_status(capture.responses[1]), 0x00);
    REQUIRE_EQ(response_status(capture.responses[2]), 0x00);
}

static void test_sm_stream_fragmented_and_coalesced(void)
{
    reset_state();
    clock_protocol_stream_t stream = {};
    clock_protocol_stream_init(&stream);
    stream_capture_t capture;
    const uint8_t first[] = {'/', 'T', 'A', 0, 'S', 'M', '0'};
    clock_protocol_stream_result_t result =
        clock_protocol_stream_consume(&stream,
                                      first,
                                      sizeof(first),
                                      capture_frame,
                                      &capture);
    REQUIRE_EQ(result.frames_dispatched, 0u);

    const uint8_t second[] = {'2', '\\'};
    result = clock_protocol_stream_consume(&stream,
                                           second,
                                           sizeof(second),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 1u);
    require_sm_response(capture.responses[0], 2, 0x00);

    std::vector<uint8_t> sm_3 = make_sm(3);
    std::vector<uint8_t> lp_1 = make_lp(1);
    std::vector<uint8_t> combined = sm_3;
    combined.insert(combined.end(), lp_1.begin(), lp_1.end());
    result = clock_protocol_stream_consume(&stream,
                                           combined.data(),
                                           combined.size(),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 2u);
    require_sm_response(capture.responses[1], 3, 0x00);
    REQUIRE_EQ(response_status(capture.responses[2]), 0x00);

    std::vector<uint8_t> rc = {'/', 'T', 'A', 0, 'R', 'C', '\\'};
    std::vector<uint8_t> sm_4 = make_sm(4);
    combined = rc;
    combined.insert(combined.end(), sm_4.begin(), sm_4.end());
    result = clock_protocol_stream_consume(&stream,
                                           combined.data(),
                                           combined.size(),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 2u);
    REQUIRE_EQ(capture.results[3], 17);
    require_sm_response(capture.responses[4], 4, 0x00);
    REQUIRE_EQ(protocol_test_display_mode(), 4);
}

static void test_rm_stream_fragmented_and_coalesced(void)
{
    reset_state();
    require_sm_response(dispatch(make_sm(2)), 2, 0x00);
    const int save_count = protocol_test_mode_save_count();
    const int reset_count = protocol_test_mode_reset_count();

    clock_protocol_stream_t stream = {};
    clock_protocol_stream_init(&stream);
    stream_capture_t capture;
    const uint8_t first[] = {'/', 'T', 'A'};
    clock_protocol_stream_result_t result =
        clock_protocol_stream_consume(&stream,
                                      first,
                                      sizeof(first),
                                      capture_frame,
                                      &capture);
    REQUIRE_EQ(result.frames_dispatched, 0u);

    const uint8_t second[] = {0, 'R', 'M', '\\'};
    result = clock_protocol_stream_consume(&stream,
                                           second,
                                           sizeof(second),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 1u);
    require_rm_response(capture.responses[0], 2, 0x00);

    std::vector<uint8_t> combined = make_rm();
    std::vector<uint8_t> lp_1 = make_lp(1);
    combined.insert(combined.end(), lp_1.begin(), lp_1.end());
    result = clock_protocol_stream_consume(&stream,
                                           combined.data(),
                                           combined.size(),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 2u);
    require_rm_response(capture.responses[1], 2, 0x00);
    REQUIRE_EQ(response_status(capture.responses[2]), 0x00);
    REQUIRE_EQ(protocol_test_display_mode(), 2);
    REQUIRE_EQ(protocol_test_mode_save_count(), save_count);
    REQUIRE_EQ(protocol_test_mode_reset_count(), reset_count);
}

static void test_stream_overflow_and_recovery(void)
{
    reset_state();
    clock_protocol_stream_t stream = {};
    clock_protocol_stream_init(&stream);
    stream_capture_t capture;
    std::vector<uint8_t> oversized(
        CLOCK_PROTOCOL_STREAM_CAPACITY + 1, 'X');

    clock_protocol_stream_result_t result =
        clock_protocol_stream_consume(&stream,
                                      oversized.data(),
                                      oversized.size(),
                                      capture_frame,
                                      &capture);
    REQUIRE(result.overflowed);
    REQUIRE(capture.frames.empty());

    const uint8_t terminator = '\\';
    clock_protocol_stream_consume(&stream,
                                  &terminator,
                                  1,
                                  capture_frame,
                                  &capture);

    std::vector<uint8_t> request = make_lp(1);
    result = clock_protocol_stream_consume(&stream,
                                           request.data(),
                                           request.size(),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 1u);
    REQUIRE_EQ(response_status(capture.responses.back()), 0x00);
}

static void test_stream_keep_alive(void)
{
    reset_state();
    clock_protocol_stream_t stream = {};
    clock_protocol_stream_init(&stream);
    stream_capture_t capture;
    const uint8_t keep_alive[] = {0, 0, 0};

    clock_protocol_stream_result_t result =
        clock_protocol_stream_consume(&stream,
                                      keep_alive,
                                      sizeof(keep_alive),
                                      capture_frame,
                                      &capture);
    REQUIRE_EQ(result.frames_dispatched, 1u);
    REQUIRE_EQ(capture.frames[0].size(), 3u);
    REQUIRE_EQ(capture.results[0], -1);
}

static void test_stream_preserves_legacy_raw_delimiter_payload(void)
{
    reset_state();
    clock_protocol_stream_t stream = {};
    clock_protocol_stream_init(&stream);
    stream_capture_t capture;
    const uint8_t ct[] = {
        '/', 'T', 'A', 0, 'C', 'T', 0, 0x5c, '\\'
    };

    clock_protocol_stream_result_t result =
        clock_protocol_stream_consume(&stream,
                                      ct,
                                      sizeof(ct),
                                      capture_frame,
                                      &capture);
    REQUIRE_EQ(result.frames_dispatched, 1u);
    REQUIRE_EQ(capture.frames[0].size(), sizeof(ct));
    REQUIRE_EQ(capture.frames[0][7], 0x5c);
    REQUIRE_EQ(capture.results[0], 0);

    const uint8_t ca[] = {
        '/', 'T', 'A', 0, 'C', 'A', 9, 7, 30, 0x5c, 0x5c, '\\'
    };
    result = clock_protocol_stream_consume(&stream,
                                           ca,
                                           sizeof(ca),
                                           capture_frame,
                                           &capture);
    REQUIRE_EQ(result.frames_dispatched, 1u);
    REQUIRE_EQ(capture.frames[1].size(), sizeof(ca));
    REQUIRE_EQ(capture.results[1], 8);
}

static void run_test(void (*test)(void), const char *name)
{
    test();
    s_test_count++;
    std::printf("PASS %s\n", name);
}

int main(void)
{
    run_test(test_lp_supported_modes_and_complete_sorted_ascii_response,
             "LP supported modes, complete sorted ASCII response");
    run_test(test_lp_mode_4_error_shape, "LP mode 4 error");
    run_test(test_cp_supported_modes_and_readback, "CP supported modes and LP readback");
    run_test(test_cp_validation_statuses, "CP validation statuses");
    run_test(test_cp_nvs_failure_and_unchanged_skip, "CP persistence semantics");
    run_test(test_cp_ascii_005cff_delimiter_safety, "CP ASCII 005CFF safety");
    run_test(test_dp_restores_only_selected_mode, "DP selected mode restore");
    run_test(test_existing_rc_and_alarm_commands, "existing RC CA LA DA commands");
    run_test(test_sm_supported_modes_update_and_save, "SM modes 1 through 4");
    run_test(test_sm_validation_has_no_side_effects, "SM validation");
    run_test(test_sm_apply_failures_are_reported, "SM apply failures");
    run_test(test_nm_behavior_and_response_are_unchanged, "NM unchanged");
    run_test(test_rm_reports_modes_without_side_effects, "RM modes 1 through 4");
    run_test(test_rm_internal_failure_and_board_validation,
             "RM internal failure and board validation");
    run_test(test_stream_fragmentation_and_partial_wait, "stream fragmentation");
    run_test(test_stream_coalesced_and_multiple_frames, "stream coalescing");
    run_test(test_sm_stream_fragmented_and_coalesced,
             "SM stream fragmentation and coalescing");
    run_test(test_rm_stream_fragmented_and_coalesced,
             "RM stream fragmentation and coalescing");
    run_test(test_stream_overflow_and_recovery, "stream overflow recovery");
    run_test(test_stream_keep_alive, "stream keep-alive");
    run_test(test_stream_preserves_legacy_raw_delimiter_payload,
             "stream legacy raw delimiter payload");

    std::printf("All %d palette protocol tests passed\n", s_test_count);
    return 0;
}
