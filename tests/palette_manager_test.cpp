#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clock_palette.h"
#include "esp_err.h"

void palette_test_reset_nvs(void);
void palette_test_reset_counters(void);
void palette_test_set_commit_result(esp_err_t result);
int palette_test_set_count(void);
int palette_test_commit_count(void);
int palette_test_erase_key_count(void);
int palette_test_erase_all_count(void);
void palette_test_store_raw_blob(const char *namespace_name,
                                 const char *key,
                                 const std::vector<uint8_t> &blob);
bool palette_test_read_raw_blob(const char *namespace_name,
                                const char *key,
                                std::vector<uint8_t> *out_blob);

static int s_test_count = 0;

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

static bool rgb_equal(clock_rgb_t actual,
                      uint8_t r,
                      uint8_t g,
                      uint8_t b)
{
    return actual.r == r && actual.g == g && actual.b == b;
}

static clock_mode_palette_t factory_palette(uint8_t mode)
{
    clock_mode_palette_t palette = {};
    REQUIRE(clock_palette_get_factory_snapshot(mode, &palette));
    return palette;
}

static clock_mode_palette_t active_palette(uint8_t mode)
{
    clock_mode_palette_t palette = {};
    REQUIRE(clock_palette_get_mode_snapshot(mode, &palette));
    return palette;
}

static clock_rgb_t color_for(const clock_mode_palette_t &palette, uint8_t role)
{
    clock_rgb_t color = {};
    REQUIRE(clock_palette_snapshot_get_color(&palette, role, &color));
    return color;
}

static void set_color(clock_mode_palette_t *palette,
                      uint8_t role,
                      clock_rgb_t color)
{
    REQUIRE(palette != nullptr);

    for (uint8_t i = 0; i < palette->count; ++i) {
        if (palette->entries[i].role == role) {
            palette->entries[i].color = color;
            return;
        }
    }

    REQUIRE(false);
}

static void reset_palette_state(void)
{
    palette_test_reset_nvs();
    REQUIRE_EQ(clock_palette_init(), ESP_OK);
    palette_test_reset_counters();
}

static uint32_t test_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

static void refresh_crc(std::vector<uint8_t> *blob)
{
    REQUIRE(blob != nullptr);
    REQUIRE(blob->size() >= 4);
    size_t offset = blob->size() - 4;
    uint32_t crc = test_crc32(blob->data(), offset);
    (*blob)[offset] = (uint8_t)crc;
    (*blob)[offset + 1] = (uint8_t)(crc >> 8);
    (*blob)[offset + 2] = (uint8_t)(crc >> 16);
    (*blob)[offset + 3] = (uint8_t)(crc >> 24);
}

static std::vector<uint8_t> make_saved_blob(uint8_t mode,
                                             bool change_two_roles = false)
{
    reset_palette_state();
    clock_mode_palette_t palette = factory_palette(mode);
    set_color(&palette, CLOCK_PALETTE_ROLE_TIME, {1, 2, 3});

    if (change_two_roles) {
        set_color(&palette, CLOCK_PALETTE_ROLE_DATE, {4, 5, 6});
    }

    REQUIRE_EQ(clock_palette_save_mode_override(
                   mode, palette.entries, palette.count),
               ESP_OK);

    std::vector<uint8_t> blob;
    const char *key = mode == 1 ? "m1" : mode == 2 ? "m2" : "m3";
    REQUIRE(palette_test_read_raw_blob("palette_cfg", key, &blob));
    return blob;
}

static void require_ignored_mode_1_blob(const std::vector<uint8_t> &blob)
{
    palette_test_reset_nvs();
    palette_test_store_raw_blob("palette_cfg", "m1", blob);
    REQUIRE_EQ(clock_palette_init(), ESP_OK);
    REQUIRE(rgb_equal(color_for(active_palette(1), CLOCK_PALETTE_ROLE_TIME),
                      255,
                      255,
                      255));
    REQUIRE_EQ(palette_test_set_count(), 0);
    REQUIRE_EQ(palette_test_commit_count(), 0);
    REQUIRE_EQ(palette_test_erase_key_count(), 0);
}

static void test_mode_1_factory_defaults(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = active_palette(1);
    REQUIRE_EQ(palette.count, 6);
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TIME), 255, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_DATE), 0, 255, 0));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COLD), 255, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COOL), 0, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_WARM), 255, 65, 0));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_HOT), 255, 0, 0));
}

static void test_mode_2_factory_defaults(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = active_palette(2);
    REQUIRE_EQ(palette.count, 6);
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TIME), 255, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_DATE), 0, 0, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COLD), 255, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COOL), 0, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_WARM), 255, 65, 0));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_HOT), 255, 0, 0));
}

static void test_mode_3_factory_defaults(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = active_palette(3);
    REQUIRE_EQ(palette.count, 7);
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TIME), 255, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_DATE), 0, 0, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_WEEKDAY), 0, 255, 0));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COLD), 255, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COOL), 0, 255, 255));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_WARM), 255, 65, 0));
    REQUIRE(rgb_equal(color_for(palette, CLOCK_PALETTE_ROLE_TEMPERATURE_HOT), 255, 0, 0));
}

static void test_mode_4_has_no_palette(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = {};
    REQUIRE(!clock_palette_is_supported_mode(4));
    REQUIRE(!clock_palette_get_mode_snapshot(4, &palette));
}

static void test_role_validation(void)
{
    const uint8_t common_roles[] = {
        CLOCK_PALETTE_ROLE_TIME,
        CLOCK_PALETTE_ROLE_DATE,
        CLOCK_PALETTE_ROLE_TEMPERATURE_COLD,
        CLOCK_PALETTE_ROLE_TEMPERATURE_COOL,
        CLOCK_PALETTE_ROLE_TEMPERATURE_WARM,
        CLOCK_PALETTE_ROLE_TEMPERATURE_HOT,
    };

    for (uint8_t role : common_roles) {
        REQUIRE(clock_palette_is_supported_role(1, role));
        REQUIRE(clock_palette_is_supported_role(2, role));
        REQUIRE(clock_palette_is_supported_role(3, role));
    }

    REQUIRE(!clock_palette_is_supported_role(1, CLOCK_PALETTE_ROLE_WEEKDAY));
    REQUIRE(!clock_palette_is_supported_role(2, CLOCK_PALETTE_ROLE_WEEKDAY));
    REQUIRE(clock_palette_is_supported_role(3, CLOCK_PALETTE_ROLE_WEEKDAY));
    REQUIRE(!clock_palette_is_supported_role(1, 0x7f));
    REQUIRE(!clock_palette_is_supported_role(9, CLOCK_PALETTE_ROLE_TIME));
}

static void test_complete_save_validation(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = factory_palette(1);

    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, palette.entries, palette.count - 1),
               ESP_ERR_INVALID_ARG);

    palette.entries[0].role = CLOCK_PALETTE_ROLE_WEEKDAY;
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, palette.entries, palette.count),
               ESP_ERR_INVALID_ARG);

    palette = factory_palette(1);
    palette.entries[1].role = palette.entries[0].role;
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, palette.entries, palette.count),
               ESP_ERR_INVALID_ARG);

    palette = factory_palette(1);
    REQUIRE_EQ(clock_palette_save_mode_override(
                   4, palette.entries, palette.count),
               ESP_ERR_INVALID_ARG);
    REQUIRE_EQ(palette_test_set_count(), 0);
    REQUIRE_EQ(palette_test_commit_count(), 0);
}

static void test_valid_blob_encodes_and_loads(void)
{
    std::vector<uint8_t> blob = make_saved_blob(2);
    REQUIRE_EQ(blob.size(), 14u);
    REQUIRE_EQ(blob[0], 'P');
    REQUIRE_EQ(blob[1], 'L');
    REQUIRE_EQ(blob[2], 1);
    REQUIRE_EQ(blob[3], 2);
    REQUIRE_EQ(blob[4], 1);
    REQUIRE_EQ(blob[5], 0);

    REQUIRE_EQ(clock_palette_init(), ESP_OK);
    REQUIRE(rgb_equal(color_for(active_palette(2), CLOCK_PALETTE_ROLE_TIME),
                      1,
                      2,
                      3));
}

static void test_crc_mismatch_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1);
    blob[7] ^= 0xff;
    require_ignored_mode_1_blob(blob);
}

static void test_wrong_magic_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1);
    blob[0] = 'X';
    require_ignored_mode_1_blob(blob);
}

static void test_unsupported_version_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1);
    blob[2] = 2;
    require_ignored_mode_1_blob(blob);
}

static void test_wrong_mode_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1);
    blob[3] = 2;
    require_ignored_mode_1_blob(blob);
}

static void test_duplicate_role_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1, true);
    REQUIRE_EQ(blob[4], 2);
    blob[10] = blob[6];
    refresh_crc(&blob);
    require_ignored_mode_1_blob(blob);
}

static void test_unsupported_role_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1);
    blob[6] = 0x7f;
    refresh_crc(&blob);
    require_ignored_mode_1_blob(blob);
}

static void test_invalid_count_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1);
    blob[4] = 0;
    require_ignored_mode_1_blob(blob);
}

static void test_malformed_length_rejected(void)
{
    std::vector<uint8_t> blob = make_saved_blob(1);
    blob.pop_back();
    require_ignored_mode_1_blob(blob);
}

static void test_unchanged_save_skips_nvs(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = factory_palette(1);
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, palette.entries, palette.count),
               ESP_OK);
    REQUIRE_EQ(palette_test_set_count(), 0);
    REQUIRE_EQ(palette_test_commit_count(), 0);
    REQUIRE_EQ(palette_test_erase_key_count(), 0);
}

static void test_changed_save_persists_then_updates_ram(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = factory_palette(1);
    set_color(&palette, CLOCK_PALETTE_ROLE_DATE, {9, 8, 7});
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, palette.entries, palette.count),
               ESP_OK);
    REQUIRE_EQ(palette_test_set_count(), 1);
    REQUIRE_EQ(palette_test_commit_count(), 1);
    REQUIRE(rgb_equal(color_for(active_palette(1), CLOCK_PALETTE_ROLE_DATE),
                      9,
                      8,
                      7));
    REQUIRE_EQ(clock_palette_init(), ESP_OK);
    REQUIRE(rgb_equal(color_for(active_palette(1), CLOCK_PALETTE_ROLE_DATE),
                      9,
                      8,
                      7));
}

static void test_failed_persistence_does_not_update_ram(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = factory_palette(1);
    set_color(&palette, CLOCK_PALETTE_ROLE_TIME, {3, 4, 5});
    palette_test_set_commit_result(ESP_FAIL);
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, palette.entries, palette.count),
               ESP_FAIL);
    REQUIRE(rgb_equal(color_for(active_palette(1), CLOCK_PALETTE_ROLE_TIME),
                      255,
                      255,
                      255));
    REQUIRE(!palette_test_read_raw_blob("palette_cfg", "m1", nullptr));
}

static void test_saving_factory_defaults_erases_override(void)
{
    reset_palette_state();
    clock_mode_palette_t changed = factory_palette(1);
    set_color(&changed, CLOCK_PALETTE_ROLE_TIME, {3, 4, 5});
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, changed.entries, changed.count),
               ESP_OK);

    palette_test_reset_counters();
    clock_mode_palette_t factory = factory_palette(1);
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, factory.entries, factory.count),
               ESP_OK);
    REQUIRE_EQ(palette_test_erase_key_count(), 1);
    REQUIRE_EQ(palette_test_commit_count(), 1);
    REQUIRE(!palette_test_read_raw_blob("palette_cfg", "m1", nullptr));
    REQUIRE(rgb_equal(color_for(active_palette(1), CLOCK_PALETTE_ROLE_TIME),
                      255,
                      255,
                      255));
}

static void test_restore_one_mode_only(void)
{
    reset_palette_state();
    clock_mode_palette_t mode_1 = factory_palette(1);
    clock_mode_palette_t mode_2 = factory_palette(2);
    set_color(&mode_1, CLOCK_PALETTE_ROLE_TIME, {1, 1, 1});
    set_color(&mode_2, CLOCK_PALETTE_ROLE_TIME, {2, 2, 2});
    REQUIRE_EQ(clock_palette_save_mode_override(
                   1, mode_1.entries, mode_1.count),
               ESP_OK);
    REQUIRE_EQ(clock_palette_save_mode_override(
                   2, mode_2.entries, mode_2.count),
               ESP_OK);

    REQUIRE_EQ(clock_palette_restore_mode_defaults(1), ESP_OK);
    REQUIRE(rgb_equal(color_for(active_palette(1), CLOCK_PALETTE_ROLE_TIME),
                      255,
                      255,
                      255));
    REQUIRE(rgb_equal(color_for(active_palette(2), CLOCK_PALETTE_ROLE_TIME),
                      2,
                      2,
                      2));
    REQUIRE(!palette_test_read_raw_blob("palette_cfg", "m1", nullptr));
    REQUIRE(palette_test_read_raw_blob("palette_cfg", "m2", nullptr));
}

static void test_restore_all_modes(void)
{
    reset_palette_state();
    palette_test_store_raw_blob("clock_cfg", "unrelated", {1, 2, 3});

    for (uint8_t mode = 1; mode <= 3; ++mode) {
        clock_mode_palette_t palette = factory_palette(mode);
        set_color(&palette, CLOCK_PALETTE_ROLE_TIME, {mode, mode, mode});
        REQUIRE_EQ(clock_palette_save_mode_override(
                       mode, palette.entries, palette.count),
                   ESP_OK);
    }

    palette_test_reset_counters();
    REQUIRE_EQ(clock_palette_restore_all_defaults(), ESP_OK);
    REQUIRE_EQ(palette_test_erase_all_count(), 1);
    REQUIRE_EQ(palette_test_commit_count(), 1);

    for (uint8_t mode = 1; mode <= 3; ++mode) {
        REQUIRE(rgb_equal(color_for(active_palette(mode),
                                    CLOCK_PALETTE_ROLE_TIME),
                          255,
                          255,
                          255));
    }

    REQUIRE(palette_test_read_raw_blob("clock_cfg", "unrelated", nullptr));
}

static void test_snapshot_and_temperature_thresholds(void)
{
    reset_palette_state();
    clock_mode_palette_t palette = factory_palette(3);
    set_color(&palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COLD, {1, 0, 0});
    set_color(&palette, CLOCK_PALETTE_ROLE_TEMPERATURE_COOL, {2, 0, 0});
    set_color(&palette, CLOCK_PALETTE_ROLE_TEMPERATURE_WARM, {3, 0, 0});
    set_color(&palette, CLOCK_PALETTE_ROLE_TEMPERATURE_HOT, {4, 0, 0});
    REQUIRE_EQ(clock_palette_save_mode_override(
                   3, palette.entries, palette.count),
               ESP_OK);

    clock_mode_palette_t snapshot = active_palette(3);
    clock_rgb_t color = {};
    REQUIRE(clock_palette_snapshot_get_temperature_color(&snapshot, 9.9f, &color));
    REQUIRE(rgb_equal(color, 1, 0, 0));
    REQUIRE(clock_palette_snapshot_get_temperature_color(&snapshot, 10.0f, &color));
    REQUIRE(rgb_equal(color, 2, 0, 0));
    REQUIRE(clock_palette_snapshot_get_temperature_color(&snapshot, 19.9f, &color));
    REQUIRE(rgb_equal(color, 2, 0, 0));
    REQUIRE(clock_palette_snapshot_get_temperature_color(&snapshot, 20.0f, &color));
    REQUIRE(rgb_equal(color, 3, 0, 0));
    REQUIRE(clock_palette_snapshot_get_temperature_color(&snapshot, 29.9f, &color));
    REQUIRE(rgb_equal(color, 3, 0, 0));
    REQUIRE(clock_palette_snapshot_get_temperature_color(&snapshot, 30.0f, &color));
    REQUIRE(rgb_equal(color, 4, 0, 0));
}

static void run_test(void (*test)(void), const char *name)
{
    test();
    s_test_count++;
    std::printf("PASS %s\n", name);
}

int main(void)
{
    run_test(test_mode_1_factory_defaults, "mode 1 factory defaults");
    run_test(test_mode_2_factory_defaults, "mode 2 factory defaults");
    run_test(test_mode_3_factory_defaults, "mode 3 factory defaults");
    run_test(test_mode_4_has_no_palette, "mode 4 has no palette");
    run_test(test_role_validation, "role validation");
    run_test(test_complete_save_validation, "complete save validation");
    run_test(test_valid_blob_encodes_and_loads, "valid blob encode/load");
    run_test(test_crc_mismatch_rejected, "CRC mismatch rejected");
    run_test(test_wrong_magic_rejected, "wrong magic rejected");
    run_test(test_unsupported_version_rejected, "version rejected");
    run_test(test_wrong_mode_rejected, "wrong mode rejected");
    run_test(test_duplicate_role_rejected, "duplicate role rejected");
    run_test(test_unsupported_role_rejected, "unsupported role rejected");
    run_test(test_invalid_count_rejected, "invalid count rejected");
    run_test(test_malformed_length_rejected, "malformed length rejected");
    run_test(test_unchanged_save_skips_nvs, "unchanged save skips NVS");
    run_test(test_changed_save_persists_then_updates_ram, "changed save persists");
    run_test(test_failed_persistence_does_not_update_ram, "failed save preserves RAM");
    run_test(test_saving_factory_defaults_erases_override, "factory save erases override");
    run_test(test_restore_one_mode_only, "restore one mode");
    run_test(test_restore_all_modes, "restore all modes");
    run_test(test_snapshot_and_temperature_thresholds, "snapshot temperature colors");

    std::printf("All %d palette tests passed\n", s_test_count);
    return 0;
}
