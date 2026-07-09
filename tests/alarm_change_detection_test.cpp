#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clock_alarm.h"
#include "clock_protocol.h"
#include "esp_err.h"

extern int64_t g_alarm_test_now_us;
extern int g_alarm_test_save_count;
extern size_t g_alarm_test_last_save_size;

void alarm_test_reset_stubs(void);
void alarm_test_reset_save_count(void);
const unsigned char *alarm_test_last_save_blob(void);

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
#define REQUIRE_NE(actual, expected) REQUIRE((actual) != (expected))

static clock_alarm_test_state_t alarm_state(void)
{
    clock_alarm_test_state_t state = {};
    REQUIRE(clock_alarm_test_get_state(&state));
    return state;
}

static bool alarms_equal(const ethernet_alarm_t &left,
                         const ethernet_alarm_t &right)
{
    return left.configured == right.configured &&
           left.alarm_id == right.alarm_id &&
           left.time_hh == right.time_hh &&
           left.time_mm == right.time_mm &&
           left.frequency == right.frequency &&
           left.duration_effect == right.duration_effect;
}

static bool alarm_persisted_fields_empty(const ethernet_alarm_t &alarm)
{
    return !alarm.configured &&
           alarm.alarm_id == 0 &&
           alarm.time_hh == 0 &&
           alarm.time_mm == 0 &&
           alarm.frequency == 0 &&
           alarm.duration_effect == 0;
}

static ethernet_alarm_t saved_alarm(uint8_t alarm_id)
{
    ethernet_alarm_t alarm = {};
    const unsigned char *blob = alarm_test_last_save_blob();
    std::memcpy(&alarm,
                blob + ((alarm_id - 1) * sizeof(ethernet_alarm_t)),
                sizeof(alarm));
    return alarm;
}

static void require_da_ack(const uint8_t *response,
                           uint8_t board_id,
                           uint8_t alarm_id)
{
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], board_id);
    REQUIRE_EQ(response[4], 'd');
    REQUIRE_EQ(response[5], 'a');
    REQUIRE_EQ(response[6], alarm_id);
    REQUIRE_EQ(response[7], '\\');
}

static void require_alarm_read_empty(uint8_t alarm_id)
{
    ethernet_alarm_t alarm = {};
    REQUIRE(clock_alarm_read(alarm_id, &alarm));
    REQUIRE(!alarm.configured);
    REQUIRE_EQ(alarm.alarm_id, alarm_id);
    REQUIRE_EQ(alarm.time_hh, 0);
    REQUIRE_EQ(alarm.time_mm, 0);
    REQUIRE_EQ(alarm.frequency, 0);
    REQUIRE_EQ(alarm.duration_effect, 0);
}

static int send_da(uint8_t alarm_id, uint8_t *response, size_t response_size)
{
    uint8_t request[8] = {
        '/', 'T', 'A', 0x00, 'D', 'A', alarm_id, '\\'
    };

    return clock_protocol_rx_callback(request,
                                      sizeof(request),
                                      response,
                                      static_cast<int>(response_size));
}

static void reset_alarm_state(void)
{
    clock_alarm_clear_all_ram();
    alarm_test_reset_stubs();
}

static void seed_clean_alarm(uint8_t alarm_id,
                             uint8_t hour,
                             uint8_t minute,
                             uint8_t frequency,
                             uint8_t duration_effect)
{
    reset_alarm_state();
    g_alarm_test_now_us = 1000;

    REQUIRE_EQ(clock_alarm_store_from_ca(alarm_id,
                                         hour,
                                         minute,
                                         frequency,
                                         duration_effect),
               ESP_OK);

    clock_alarm_test_state_t dirty_state = alarm_state();
    REQUIRE(dirty_state.dirty);

    g_alarm_test_now_us = dirty_state.dirty_until_us;
    clock_alarm_process_deferred_save();

    clock_alarm_test_state_t clean_state = alarm_state();
    REQUIRE(!clean_state.dirty);
    REQUIRE_EQ(g_alarm_test_save_count, 1);
    REQUIRE_EQ(g_alarm_test_last_save_size,
               sizeof(ethernet_alarm_t) * MAX_ETH_ALARMS);

    alarm_test_reset_save_count();
}

static void flush_deferred_alarm_save(void)
{
    clock_alarm_test_state_t dirty_state = alarm_state();
    REQUIRE(dirty_state.dirty);

    g_alarm_test_now_us = dirty_state.dirty_until_us;
    clock_alarm_process_deferred_save();

    REQUIRE(!alarm_state().dirty);
}

static void require_changed_ca_marks_dirty(uint8_t hour,
                                           uint8_t minute,
                                           uint8_t frequency,
                                           uint8_t duration_effect)
{
    seed_clean_alarm(9, 9, 6, 0x88, 0x42);

    clock_alarm_test_state_t before = alarm_state();
    g_alarm_test_now_us = before.dirty_until_us + 5000;

    REQUIRE_EQ(clock_alarm_store_from_ca(9,
                                         hour,
                                         minute,
                                         frequency,
                                         duration_effect),
               ESP_OK);

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE(after.dirty);
    REQUIRE_EQ(after.dirty_until_us, g_alarm_test_now_us + 1000000);
    REQUIRE_EQ(g_alarm_test_save_count, 0);
}

static void test_identical_ca_does_not_mark_dirty_or_schedule_save(void)
{
    seed_clean_alarm(9, 9, 6, 0x88, 0x42);

    ethernet_alarm_t before_alarm = {};
    REQUIRE(clock_alarm_read(9, &before_alarm));

    clock_alarm_test_state_t before = alarm_state();
    g_alarm_test_now_us = before.dirty_until_us + 5000;

    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 6, 0x88, 0x42), ESP_OK);

    ethernet_alarm_t after_alarm = {};
    REQUIRE(clock_alarm_read(9, &after_alarm));
    REQUIRE(alarms_equal(before_alarm, after_alarm));

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE(!after.dirty);
    REQUIRE_EQ(after.dirty_until_us, before.dirty_until_us);
    REQUIRE_EQ(g_alarm_test_save_count, 0);

    g_alarm_test_now_us += 2000000;
    clock_alarm_process_deferred_save();
    REQUIRE_EQ(g_alarm_test_save_count, 0);
}

static void test_changed_hour_marks_dirty(void)
{
    require_changed_ca_marks_dirty(10, 6, 0x88, 0x42);
}

static void test_changed_minute_marks_dirty(void)
{
    require_changed_ca_marks_dirty(9, 7, 0x88, 0x42);
}

static void test_changed_frequency_marks_dirty(void)
{
    require_changed_ca_marks_dirty(9, 6, 0x08, 0x42);
}

static void test_changed_duration_effect_marks_dirty(void)
{
    require_changed_ca_marks_dirty(9, 6, 0x88, 0x43);
}

static void test_unchanged_ca_still_generates_ack(void)
{
    seed_clean_alarm(9, 9, 6, 0x88, 0x42);

    uint8_t request[12] = {
        '/', 'T', 'A', 0x00, 'C', 'A', 9, 9, 6, 0x88, 0x42, '\\'
    };
    uint8_t response[16] = {};

    int response_len = clock_protocol_rx_callback(request,
                                                  sizeof(request),
                                                  response,
                                                  sizeof(response));

    REQUIRE_EQ(response_len, 8);
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], 0x00);
    REQUIRE_EQ(response[4], 'c');
    REQUIRE_EQ(response[5], 'a');
    REQUIRE_EQ(response[6], 9);
    REQUIRE_EQ(response[7], '\\');

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE(!after.dirty);
    REQUIRE_EQ(g_alarm_test_save_count, 0);
}

static void test_la_remains_read_only(void)
{
    seed_clean_alarm(9, 9, 6, 0x08, 0x42);

    clock_alarm_test_state_t before = alarm_state();
    uint8_t request[8] = {
        '/', 'T', 'A', 0x00, 'L', 'A', 9, '\\'
    };
    uint8_t response[16] = {};

    int response_len = clock_protocol_rx_callback(request,
                                                  sizeof(request),
                                                  response,
                                                  sizeof(response));

    REQUIRE_EQ(response_len, 12);
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], 0x00);
    REQUIRE_EQ(response[4], 'l');
    REQUIRE_EQ(response[5], 'a');
    REQUIRE_EQ(response[6], 9);
    REQUIRE_EQ(response[7], 9);
    REQUIRE_EQ(response[8], 6);
    REQUIRE_EQ(response[9], 0x08);
    REQUIRE_EQ(response[10], 0x42);
    REQUIRE_EQ(response[11], '\\');

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE_EQ(after.dirty, before.dirty);
    REQUIRE_EQ(after.dirty_until_us, before.dirty_until_us);
    REQUIRE_EQ(g_alarm_test_save_count, 0);

    g_alarm_test_now_us = before.dirty_until_us + 2000000;
    clock_alarm_process_deferred_save();
    REQUIRE_EQ(g_alarm_test_save_count, 0);
}

static void test_da_request_requires_exact_8_byte_frame(void)
{
    reset_alarm_state();

    uint8_t request[9] = {
        '/', 'T', 'A', 0x00, 'D', 'A', 9, '\\', 0x00
    };
    uint8_t response[16] = {};

    REQUIRE_EQ(clock_protocol_rx_callback(request, 7, response, sizeof(response)),
               -1);
    REQUIRE_EQ(clock_protocol_rx_callback(request, sizeof(request), response, sizeof(response)),
               -1);

    REQUIRE_EQ(clock_protocol_rx_callback(request, 8, response, sizeof(response)),
               8);
    require_da_ack(response, 0x00, 9);
}

static void test_da_ack_exact_8_byte_format(void)
{
    reset_alarm_state();

    uint8_t response[8] = {};

    REQUIRE_EQ(send_da(17, response, sizeof(response)), 8);
    require_da_ack(response, 0x00, 17);
}

static void require_delete_configured_alarm_clears_and_schedules(uint8_t frequency)
{
    seed_clean_alarm(9, 9, 6, frequency, 0x42);

    clock_alarm_test_state_t before = alarm_state();
    g_alarm_test_now_us = before.dirty_until_us + 5000;

    uint8_t response[8] = {};
    REQUIRE_EQ(send_da(9, response, sizeof(response)), 8);
    require_da_ack(response, 0x00, 9);

    require_alarm_read_empty(9);

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE(after.dirty);
    REQUIRE_EQ(after.dirty_until_us, g_alarm_test_now_us + 1000000);
    REQUIRE_EQ(g_alarm_test_save_count, 0);

    flush_deferred_alarm_save();

    REQUIRE_EQ(g_alarm_test_save_count, 1);
    REQUIRE_EQ(g_alarm_test_last_save_size,
               sizeof(ethernet_alarm_t) * MAX_ETH_ALARMS);
    REQUIRE(alarm_persisted_fields_empty(saved_alarm(9)));
}

static void test_da_deletes_configured_enabled_alarm(void)
{
    require_delete_configured_alarm_clears_and_schedules(0x88);
}

static void test_da_deletes_configured_disabled_alarm(void)
{
    require_delete_configured_alarm_clears_and_schedules(0x08);
}

static void test_da_empty_alarm_ack_without_dirty_or_save(void)
{
    reset_alarm_state();

    clock_alarm_test_state_t before = alarm_state();
    g_alarm_test_now_us = 2000;

    uint8_t response[8] = {};
    REQUIRE_EQ(send_da(17, response, sizeof(response)), 8);
    require_da_ack(response, 0x00, 17);

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE_EQ(after.dirty, before.dirty);
    REQUIRE_EQ(after.dirty_until_us, before.dirty_until_us);
    REQUIRE_EQ(g_alarm_test_save_count, 0);

    g_alarm_test_now_us += 2000000;
    clock_alarm_process_deferred_save();
    REQUIRE_EQ(g_alarm_test_save_count, 0);
}

static void test_da_only_deletes_requested_alarm_slot(void)
{
    reset_alarm_state();

    g_alarm_test_now_us = 1000;
    REQUIRE_EQ(clock_alarm_store_from_ca(8, 8, 6, 0x88, 0x42), ESP_OK);
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 6, 0x08, 0x43), ESP_OK);
    REQUIRE_EQ(clock_alarm_store_from_ca(10, 10, 6, 0x81, 0x44), ESP_OK);

    flush_deferred_alarm_save();
    alarm_test_reset_save_count();

    ethernet_alarm_t alarm_8_before = {};
    ethernet_alarm_t alarm_10_before = {};
    REQUIRE(clock_alarm_read(8, &alarm_8_before));
    REQUIRE(clock_alarm_read(10, &alarm_10_before));

    g_alarm_test_now_us += 5000;

    uint8_t response[8] = {};
    REQUIRE_EQ(send_da(9, response, sizeof(response)), 8);
    require_da_ack(response, 0x00, 9);

    ethernet_alarm_t alarm_8_after = {};
    ethernet_alarm_t alarm_10_after = {};
    REQUIRE(clock_alarm_read(8, &alarm_8_after));
    REQUIRE(clock_alarm_read(10, &alarm_10_after));

    REQUIRE(alarms_equal(alarm_8_before, alarm_8_after));
    REQUIRE(alarms_equal(alarm_10_before, alarm_10_after));
    require_alarm_read_empty(9);
}

static void test_la_after_da_returns_zero_payload(void)
{
    seed_clean_alarm(9, 9, 6, 0x88, 0x42);

    uint8_t da_response[8] = {};
    REQUIRE_EQ(send_da(9, da_response, sizeof(da_response)), 8);
    require_da_ack(da_response, 0x00, 9);

    uint8_t request[8] = {
        '/', 'T', 'A', 0x00, 'L', 'A', 9, '\\'
    };
    uint8_t response[16] = {};

    int response_len = clock_protocol_rx_callback(request,
                                                  sizeof(request),
                                                  response,
                                                  sizeof(response));

    REQUIRE_EQ(response_len, 12);
    REQUIRE_EQ(response[0], '/');
    REQUIRE_EQ(response[1], 't');
    REQUIRE_EQ(response[2], 'a');
    REQUIRE_EQ(response[3], 0x00);
    REQUIRE_EQ(response[4], 'l');
    REQUIRE_EQ(response[5], 'a');
    REQUIRE_EQ(response[6], 9);
    REQUIRE_EQ(response[7], 0);
    REQUIRE_EQ(response[8], 0);
    REQUIRE_EQ(response[9], 0);
    REQUIRE_EQ(response[10], 0);
    REQUIRE_EQ(response[11], '\\');
}

static void test_da_does_not_send_or_trigger_es(void)
{
    reset_alarm_state();

    g_alarm_test_now_us = 1000;
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 6, 0x88, 0x42), ESP_OK);
    REQUIRE_EQ(clock_alarm_store_from_ca(10, 10, 6, 0x88, 0x42), ESP_OK);

    flush_deferred_alarm_save();
    alarm_test_reset_save_count();

    uint8_t response[8] = {};
    REQUIRE_EQ(send_da(9, response, sizeof(response)), 8);
    require_da_ack(response, 0x00, 9);

    clock_alarm_test_state_t after_da = alarm_state();
    REQUIRE(!after_da.full_replacement_armed);

    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 7, 0x88, 0x42), ESP_OK);

    ethernet_alarm_t alarm_10 = {};
    REQUIRE(clock_alarm_read(10, &alarm_10));
    REQUIRE(alarm_10.configured);
}

static void test_da_does_not_alter_es_full_replacement_behavior(void)
{
    reset_alarm_state();

    g_alarm_test_now_us = 1000;
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 6, 0x88, 0x42), ESP_OK);
    REQUIRE_EQ(clock_alarm_store_from_ca(10, 10, 6, 0x88, 0x42), ESP_OK);

    flush_deferred_alarm_save();
    alarm_test_reset_save_count();

    clock_alarm_arm_full_replacement();
    REQUIRE(alarm_state().full_replacement_armed);

    uint8_t response[8] = {};
    REQUIRE_EQ(send_da(9, response, sizeof(response)), 8);
    require_da_ack(response, 0x00, 9);
    REQUIRE(alarm_state().full_replacement_armed);

    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 7, 0x88, 0x42), ESP_OK);

    clock_alarm_test_state_t after_ca = alarm_state();
    REQUIRE(!after_ca.full_replacement_armed);

    ethernet_alarm_t alarm_10 = {};
    REQUIRE(clock_alarm_read(10, &alarm_10));
    REQUIRE(!alarm_10.configured);
    REQUIRE_EQ(alarm_10.time_hh, 0);
    REQUIRE_EQ(alarm_10.time_mm, 0);
    REQUIRE_EQ(alarm_10.frequency, 0);
    REQUIRE_EQ(alarm_10.duration_effect, 0);
}

static void test_da_rejects_invalid_alarm_ids(void)
{
    reset_alarm_state();

    uint8_t response[8] = {};

    REQUIRE_EQ(send_da(0, response, sizeof(response)), -1);
    REQUIRE_EQ(send_da(MAX_ETH_ALARMS + 1, response, sizeof(response)), -1);

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE(!after.dirty);
    REQUIRE_EQ(g_alarm_test_save_count, 0);
}

static void test_da_partial_or_coalesced_frames_have_no_side_effects(void)
{
    seed_clean_alarm(9, 9, 6, 0x88, 0x42);

    clock_alarm_test_state_t before = alarm_state();
    uint8_t request[16] = {
        '/', 'T', 'A', 0x00, 'D', 'A', 9, '\\',
        '/', 'T', 'A', 0x00, 'D', 'A', 9, '\\'
    };
    uint8_t response[16] = {};

    REQUIRE_EQ(clock_protocol_rx_callback(request, 7, response, sizeof(response)),
               -1);
    REQUIRE_EQ(clock_protocol_rx_callback(request, sizeof(request), response, sizeof(response)),
               -1);

    ethernet_alarm_t alarm = {};
    REQUIRE(clock_alarm_read(9, &alarm));
    REQUIRE(alarm.configured);

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE_EQ(after.dirty, before.dirty);
    REQUIRE_EQ(after.dirty_until_us, before.dirty_until_us);
    REQUIRE_EQ(g_alarm_test_save_count, 0);
}

static void test_multiple_changed_ca_operations_coalesce(void)
{
    reset_alarm_state();

    g_alarm_test_now_us = 1000;
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 6, 0x88, 0x42), ESP_OK);
    const int64_t first_deadline = alarm_state().dirty_until_us;

    g_alarm_test_now_us = 400000;
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 7, 0x88, 0x42), ESP_OK);
    const int64_t second_deadline = alarm_state().dirty_until_us;

    g_alarm_test_now_us = 800000;
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 7, 0x08, 0x42), ESP_OK);
    const int64_t third_deadline = alarm_state().dirty_until_us;

    REQUIRE_NE(first_deadline, second_deadline);
    REQUIRE_NE(second_deadline, third_deadline);

    g_alarm_test_now_us = first_deadline;
    clock_alarm_process_deferred_save();
    REQUIRE(alarm_state().dirty);
    REQUIRE_EQ(g_alarm_test_save_count, 0);

    g_alarm_test_now_us = third_deadline;
    clock_alarm_process_deferred_save();
    REQUIRE(!alarm_state().dirty);
    REQUIRE_EQ(g_alarm_test_save_count, 1);
}

static void test_es_full_replacement_still_clears_other_alarms(void)
{
    reset_alarm_state();

    g_alarm_test_now_us = 1000;
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 6, 0x88, 0x42), ESP_OK);
    REQUIRE_EQ(clock_alarm_store_from_ca(10, 10, 6, 0x88, 0x42), ESP_OK);

    clock_alarm_test_state_t dirty_state = alarm_state();
    g_alarm_test_now_us = dirty_state.dirty_until_us;
    clock_alarm_process_deferred_save();
    alarm_test_reset_save_count();

    clock_alarm_arm_full_replacement();
    g_alarm_test_now_us += 5000;
    REQUIRE_EQ(clock_alarm_store_from_ca(9, 9, 6, 0x88, 0x42), ESP_OK);

    clock_alarm_test_state_t after = alarm_state();
    REQUIRE(after.dirty);

    ethernet_alarm_t alarm_10 = {};
    REQUIRE(clock_alarm_read(10, &alarm_10));
    REQUIRE(!alarm_10.configured);
    REQUIRE_EQ(alarm_10.time_hh, 0);
    REQUIRE_EQ(alarm_10.time_mm, 0);
    REQUIRE_EQ(alarm_10.frequency, 0);
    REQUIRE_EQ(alarm_10.duration_effect, 0);
}

int main(void)
{
    test_identical_ca_does_not_mark_dirty_or_schedule_save();
    test_changed_hour_marks_dirty();
    test_changed_minute_marks_dirty();
    test_changed_frequency_marks_dirty();
    test_changed_duration_effect_marks_dirty();
    test_unchanged_ca_still_generates_ack();
    test_la_remains_read_only();
    test_da_request_requires_exact_8_byte_frame();
    test_da_ack_exact_8_byte_format();
    test_da_deletes_configured_enabled_alarm();
    test_da_deletes_configured_disabled_alarm();
    test_da_empty_alarm_ack_without_dirty_or_save();
    test_da_only_deletes_requested_alarm_slot();
    test_la_after_da_returns_zero_payload();
    test_da_does_not_send_or_trigger_es();
    test_da_does_not_alter_es_full_replacement_behavior();
    test_da_rejects_invalid_alarm_ids();
    test_da_partial_or_coalesced_frames_have_no_side_effects();
    test_multiple_changed_ca_operations_coalesce();
    test_es_full_replacement_still_clears_other_alarms();

    std::puts("alarm change detection tests passed");
    return 0;
}
