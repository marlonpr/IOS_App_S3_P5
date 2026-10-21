#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The minimal host stubs omit these ESP-IDF definitions.
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_NOT_FOUND 0x105

static FILE *test_logo_fopen(const char *path, const char *mode);

// Exercise the real loader and playback state with an in-memory SD file.
// Including the implementation keeps test hooks out of the firmware API.
#define fopen test_logo_fopen
#include "../main/clock_logo_manager.cpp"
#undef fopen

static int64_t s_test_now_us = 1000000;
static std::vector<uint8_t> s_test_file;

#define REQUIRE(expression)                                                   \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

int64_t esp_timer_get_time(void)
{
    return s_test_now_us;
}

bool clock_sd_card_is_mounted(void)
{
    return true;
}

static FILE *test_logo_fopen(const char *path, const char *mode)
{
    REQUIRE(std::strcmp(path, "/sdcard/logo.lgo") == 0);
    REQUIRE(std::strcmp(mode, "rb") == 0);
    FILE *file = std::tmpfile();
    REQUIRE(file != nullptr);
    REQUIRE(std::fwrite(s_test_file.data(), 1, s_test_file.size(), file) ==
            s_test_file.size());
    std::rewind(file);
    return file;
}

static void write_u16(uint8_t *dest, uint16_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *dest, uint32_t value)
{
    for (int byte = 0; byte < 4; ++byte) {
        dest[byte] = (uint8_t)(value >> (byte * 8));
    }
}

static std::array<uint8_t, CLOCK_LOGO_HEADER_BYTES> sequence_header(uint8_t count)
{
    std::array<uint8_t, CLOCK_LOGO_HEADER_BYTES> header = {};
    std::memcpy(header.data(), "LGS1", 4);
    write_u16(&header[4], 64);
    write_u16(&header[6], 32);
    header[8] = 1;
    header[9] = count;
    write_u16(&header[10], 3000);
    write_u32(&header[12], (uint32_t)count * 6144u);
    return header;
}

static void test_headers(void)
{
    clock_logo_file_info_t info = {};
    for (uint8_t count : {4, 5}) {
        const auto header = sequence_header(count);
        REQUIRE(clock_logo_parse_file_header(header.data(), &info));
        REQUIRE(info.frame_count == count);
        REQUIRE(info.frame_duration_ms == 3000);
        REQUIRE(info.payload_length == (uint32_t)count * 6144u);
    }
    for (uint8_t count : {0, 1, 3, 6, 255}) {
        const auto header = sequence_header(count);
        REQUIRE(!clock_logo_parse_file_header(header.data(), &info));
    }
    auto header = sequence_header(5);
    write_u32(&header[12], 24576);
    REQUIRE(!clock_logo_parse_file_header(header.data(), &info));
    header = sequence_header(4);
    write_u32(&header[12], 30720);
    REQUIRE(!clock_logo_parse_file_header(header.data(), &info));
    header = sequence_header(5);
    write_u16(&header[10], 2000);
    REQUIRE(!clock_logo_parse_file_header(header.data(), &info));
    header = sequence_header(1);
    std::memcpy(header.data(), "LGO1", 4);
    header[9] = 0;
    write_u16(&header[10], 0);
    REQUIRE(clock_logo_parse_file_header(header.data(), &info));
    REQUIRE(info.frame_count == 1 && info.payload_length == 6144);
    std::puts("PASS five-frame, legacy four-frame, single-image and invalid headers");
}

static void require_frame(uint32_t expected)
{
    const uint32_t *pixels = clock_logo_lock_pixels();
    REQUIRE(pixels != nullptr);
    REQUIRE(pixels[0] == expected);
    REQUIRE(pixels[CLOCK_LOGO_PIXEL_COUNT - 1] == expected);
    clock_logo_unlock_pixels();
}

static void test_load_and_loop(uint8_t count)
{
    auto header = sequence_header(count);
    std::vector<uint8_t> payload((size_t)count * CLOCK_LOGO_PAYLOAD_BYTES, 0);
    for (uint8_t frame = 0; frame < count; ++frame) {
        for (size_t pixel = 0; pixel < CLOCK_LOGO_PIXEL_COUNT; ++pixel) {
            payload[(size_t)frame * CLOCK_LOGO_PAYLOAD_BYTES + pixel * 3] =
                (uint8_t)(frame + 1);
        }
    }
    write_u32(&header[16], clock_logo_crc32_finalize(clock_logo_crc32_update(
        0xFFFFFFFFu, payload.data(), payload.size())));
    s_test_file.assign(header.begin(), header.end());
    s_test_file.insert(s_test_file.end(), payload.begin(), payload.end());
    s_test_now_us += 1000000;
    REQUIRE(clock_logo_reload_from_sd() == ESP_OK);
    const int64_t start_us = s_test_now_us;
    for (int64_t elapsed_us = 0; elapsed_us < 1000000000; elapsed_us += 1000000) {
        s_test_now_us = start_us + elapsed_us;
        require_frame((uint32_t)((elapsed_us / 3000000) % count + 1) << 16);
    }
    s_test_now_us = start_us + 999999999LL;
    require_frame((uint32_t)((999999999LL / 3000000) % count + 1) << 16);
    clock_logo_restart_sequence();
    require_frame(1u << 16);
    std::printf("PASS %u-frame SD load, 1000-second looping and frame-1 restart\n",
                (unsigned)count);
}

int main(void)
{
    test_headers();
    clock_logo_init();
    test_load_and_loop(4);
    test_load_and_loop(5);
    std::free(s_sequence_pixels);
    s_sequence_pixels = nullptr;
    std::puts("All logo sequence tests passed");
}
