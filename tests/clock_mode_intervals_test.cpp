#include <cstdio>
#include <cstdlib>

#include "clock_modes.h"

void interval_test_reset_environment(void);
void interval_test_reset_counters(void);
void interval_test_store_u16(const char *key, uint16_t value);
void interval_test_store_corrupt(const char *key);
void interval_test_set_commit_result(esp_err_t result);
int interval_test_set_count(void);
int interval_test_commit_count(void);
void interval_test_set_time_us(int64_t now_us);
void interval_test_advance_time_us(int64_t delta_us);

namespace {

int s_test_count = 0;

void fail_at(int line, const char *expression)
{
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::exit(1);
}

#define REQUIRE(expression)                    \
    do {                                       \
        if (!(expression)) {                   \
            fail_at(__LINE__, #expression);    \
        }                                      \
    } while (0)

#define REQUIRE_EQ(actual, expected) REQUIRE((actual) == (expected))

void require_intervals(uint16_t logo_seconds, uint16_t mode_seconds)
{
    const clock_mode_intervals_t active = clock_modes_get_intervals();
    REQUIRE_EQ(active.logo_seconds, logo_seconds);
    REQUIRE_EQ(active.mode_seconds, mode_seconds);
}

void reset_with_missing_nvs(void)
{
    interval_test_reset_environment();
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    interval_test_reset_counters();
}

void test_defaults_and_validation(void)
{
    reset_with_missing_nvs();
    require_intervals(3, 21);

    const clock_mode_intervals_t minimum = {1, 1};
    const clock_mode_intervals_t maximum = {9999, 9999};
    const clock_mode_intervals_t zero_logo = {0, 21};
    const clock_mode_intervals_t zero_mode = {3, 0};
    const clock_mode_intervals_t high_logo = {10000, 21};
    const clock_mode_intervals_t high_mode = {3, 10000};
    REQUIRE(clock_modes_validate_intervals(&minimum));
    REQUIRE(clock_modes_validate_intervals(&maximum));
    REQUIRE(!clock_modes_validate_intervals(&zero_logo));
    REQUIRE(!clock_modes_validate_intervals(&zero_mode));
    REQUIRE(!clock_modes_validate_intervals(&high_logo));
    REQUIRE(!clock_modes_validate_intervals(&high_mode));
}

void test_fixed_sequence_uses_shared_intervals(void)
{
    reset_with_missing_nvs();
    interval_test_set_time_us(1000000);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_LOGO);
    interval_test_advance_time_us(2999999);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_LOGO);
    interval_test_advance_time_us(1);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_SCREEN);
    interval_test_advance_time_us(20999999);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_SCREEN);
    interval_test_advance_time_us(1);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_LOGO);
}

void test_rotation_sequence_uses_shared_intervals(void)
{
    reset_with_missing_nvs();
    interval_test_set_time_us(1000000);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_LOGO);
    interval_test_advance_time_us(3000000);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_MODE_1);
    interval_test_advance_time_us(21000000);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_MODE_2);
    interval_test_advance_time_us(21000000);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_MODE_3);
    interval_test_advance_time_us(21000000);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_LOGO);
}

void test_set_updates_pair_resets_sequences_and_skips_unchanged_write(void)
{
    reset_with_missing_nvs();
    const clock_mode_intervals_t custom = {5, 12};
    REQUIRE_EQ(clock_modes_set_intervals(&custom), ESP_OK);
    require_intervals(5, 12);
    REQUIRE_EQ(interval_test_set_count(), 2);
    REQUIRE_EQ(interval_test_commit_count(), 1);

    interval_test_set_time_us(2000000);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_LOGO);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_LOGO);
    interval_test_advance_time_us(5000000);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_SCREEN);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_MODE_1);

    interval_test_reset_counters();
    REQUIRE_EQ(clock_modes_set_intervals(&custom), ESP_OK);
    REQUIRE_EQ(interval_test_set_count(), 0);
    REQUIRE_EQ(interval_test_commit_count(), 0);
    REQUIRE_EQ(clock_modes_get_fixed_item(), FIXED_ITEM_LOGO);
    REQUIRE_EQ(clock_modes_get_rotation_item(), ROT_ITEM_LOGO);
}

void test_invalid_set_rejects_pair_atomically(void)
{
    reset_with_missing_nvs();
    const clock_mode_intervals_t invalid_pairs[] = {
        {0, 21},
        {3, 0},
        {10000, 21},
        {3, 10000},
    };

    for (const auto &invalid : invalid_pairs) {
        REQUIRE_EQ(clock_modes_set_intervals(&invalid), ESP_ERR_INVALID_ARG);
        require_intervals(3, 21);
    }
    REQUIRE_EQ(interval_test_set_count(), 0);
    REQUIRE_EQ(interval_test_commit_count(), 0);
}

void test_missing_corrupt_and_out_of_range_nvs_use_pair_defaults(void)
{
    interval_test_reset_environment();
    interval_test_store_u16("logo_sec", 9);
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    require_intervals(3, 21);

    interval_test_reset_environment();
    interval_test_store_corrupt("logo_sec");
    interval_test_store_u16("mode_sec", 30);
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    require_intervals(3, 21);

    interval_test_reset_environment();
    interval_test_store_u16("logo_sec", 8);
    interval_test_store_u16("mode_sec", 0);
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    require_intervals(3, 21);

    interval_test_reset_environment();
    interval_test_store_u16("logo_sec", 10000);
    interval_test_store_u16("mode_sec", 50);
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    require_intervals(3, 21);
}

void test_persisted_values_reload_and_restore_defaults(void)
{
    reset_with_missing_nvs();
    const clock_mode_intervals_t custom = {9999, 9999};
    REQUIRE_EQ(clock_modes_set_intervals(&custom), ESP_OK);
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    require_intervals(9999, 9999);

    interval_test_reset_counters();
    REQUIRE_EQ(clock_modes_restore_default_intervals(), ESP_OK);
    require_intervals(3, 21);
    REQUIRE_EQ(interval_test_set_count(), 2);
    REQUIRE_EQ(interval_test_commit_count(), 1);
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    require_intervals(3, 21);
}

void test_persistence_failure_preserves_active_and_durable_pair(void)
{
    reset_with_missing_nvs();
    const clock_mode_intervals_t stored = {7, 31};
    REQUIRE_EQ(clock_modes_set_intervals(&stored), ESP_OK);

    interval_test_set_commit_result(ESP_FAIL);
    const clock_mode_intervals_t rejected = {10, 40};
    REQUIRE_EQ(clock_modes_set_intervals(&rejected), ESP_FAIL);
    require_intervals(7, 31);

    interval_test_set_commit_result(ESP_OK);
    REQUIRE_EQ(clock_modes_init_intervals(), ESP_OK);
    require_intervals(7, 31);
}

void run_test(void (*test)(void), const char *name)
{
    test();
    s_test_count++;
    std::printf("PASS %s\n", name);
}

}  // namespace

int main(void)
{
    run_test(test_defaults_and_validation, "defaults and validation");
    run_test(test_fixed_sequence_uses_shared_intervals,
             "fixed sequence shared intervals");
    run_test(test_rotation_sequence_uses_shared_intervals,
             "rotation sequence shared intervals");
    run_test(test_set_updates_pair_resets_sequences_and_skips_unchanged_write,
             "set reset and unchanged persistence");
    run_test(test_invalid_set_rejects_pair_atomically,
             "atomic invalid set rejection");
    run_test(test_missing_corrupt_and_out_of_range_nvs_use_pair_defaults,
             "invalid NVS pair defaults");
    run_test(test_persisted_values_reload_and_restore_defaults,
             "persist reload and restore defaults");
    run_test(test_persistence_failure_preserves_active_and_durable_pair,
             "persistence failure preservation");

    std::printf("All %d display interval tests passed\n", s_test_count);
    return 0;
}
