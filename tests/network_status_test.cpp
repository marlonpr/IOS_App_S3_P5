#include <cstdio>
#include <cstdlib>

#include "clock_network.h"
#include "clock_network_indicator.h"

static void fail_at(int line, const char *expression)
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

namespace {

void require_color(clock_network_status_t status,
                   int64_t now_us,
                   uint8_t red,
                   uint8_t green,
                   uint8_t blue,
                   size_t expected_count);

void begin_and_fail_attempt(clock_network_retry_state_t *state,
                            uint64_t now_ms,
                            uint8_t expected_attempt)
{
    REQUIRE(clock_network_retry_attempt_is_due(state, now_ms));
    REQUIRE_EQ(clock_network_retry_begin_attempt(state), expected_attempt);
    REQUIRE_EQ(state->status, CLOCK_NETWORK_CONNECTING);
    REQUIRE(!state->attempt_complete);

    clock_network_retry_attempt_failed(state);
    REQUIRE_EQ(state->status, CLOCK_NETWORK_CONNECTING);
    REQUIRE(state->attempt_complete);
    REQUIRE_EQ(state->attempt, expected_attempt);
}

void require_reconnect_burst_and_cooldown_timeline(
    clock_network_mode_t mode,
    clock_network_interface_t expected_interface)
{
    REQUIRE_EQ(clock_network_selected_interface(mode), expected_interface);
    REQUIRE_EQ(CLOCK_NETWORK_RETRY_SLOT_MS, 20000UL);
    REQUIRE_EQ(CLOCK_NETWORK_ATTEMPTS_PER_BURST, 3U);
    REQUIRE_EQ(CLOCK_NETWORK_FINAL_ATTEMPT_START_MS, 40000UL);
    REQUIRE_EQ(CLOCK_NETWORK_BURST_DURATION_MS, 60000UL);
    REQUIRE_EQ(CLOCK_NETWORK_COOLDOWN_MS, 300000UL);

    constexpr uint64_t kBurstStartMs = 1000;
    clock_network_retry_state_t state = {};
    clock_network_retry_state_init(&state, 0);
    REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);

    clock_network_retry_connected(&state);
    REQUIRE_EQ(state.status, CLOCK_NETWORK_ONLINE);
    require_color(state.status, 0, 0, 255, 0, 3);

    clock_network_retry_disconnected(&state, kBurstStartMs);

    REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
    REQUIRE_EQ(state.attempt, 0);
    REQUIRE_EQ(state.burst_started_ms, kBurstStartMs);
    REQUIRE(clock_network_retry_attempt_is_due(&state, kBurstStartMs));
    require_color(state.status, 0, 255, 255, 255, 3);

    begin_and_fail_attempt(&state, kBurstStartMs, 1);
    REQUIRE_EQ(state.next_action_ms,
               kBurstStartMs + CLOCK_NETWORK_RETRY_SLOT_MS);
    REQUIRE(!clock_network_retry_attempt_is_due(
        &state, kBurstStartMs + CLOCK_NETWORK_RETRY_SLOT_MS - 1));
    REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
    require_color(state.status, 0, 255, 255, 255, 3);

    begin_and_fail_attempt(
        &state,
        kBurstStartMs + CLOCK_NETWORK_RETRY_SLOT_MS,
        2);
    REQUIRE_EQ(state.next_action_ms,
               kBurstStartMs + 2ULL * CLOCK_NETWORK_RETRY_SLOT_MS);
    REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
    require_color(state.status, 0, 255, 255, 255, 3);

    begin_and_fail_attempt(
        &state,
        kBurstStartMs + 2ULL * CLOCK_NETWORK_RETRY_SLOT_MS,
        3);
    const uint64_t burst_end_ms =
        kBurstStartMs + CLOCK_NETWORK_BURST_DURATION_MS;
    REQUIRE_EQ(state.next_action_ms, burst_end_ms);
    REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
    require_color(state.status, 0, 255, 255, 255, 3);

    const uint64_t before_burst_end_ms[] = {
        kBurstStartMs + 41000,
        kBurstStartMs + 50000,
        burst_end_ms - 1,
    };
    for (uint64_t timestamp_ms : before_burst_end_ms) {
        REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
        REQUIRE_EQ(state.attempt, 3);
        REQUIRE(!clock_network_retry_attempt_is_due(&state, timestamp_ms));
        REQUIRE(!clock_network_retry_burst_is_exhausted(&state,
                                                         timestamp_ms));
        require_color(state.status, 0, 255, 255, 255, 3);
    }

    REQUIRE(!clock_network_retry_attempt_is_due(&state, burst_end_ms));
    REQUIRE(clock_network_retry_burst_is_exhausted(&state, burst_end_ms));

    clock_network_retry_enter_cooldown(&state,
                                       burst_end_ms);
    REQUIRE_EQ(state.status, CLOCK_NETWORK_OFFLINE);
    REQUIRE_EQ(state.attempt, 0);
    REQUIRE_EQ(state.next_action_ms,
               burst_end_ms + CLOCK_NETWORK_COOLDOWN_MS);
    require_color(state.status, 0, 255, 0, 0, 3);

    const uint64_t cooldown_checks_ms[] = {1000, 60000, 299000};
    for (uint64_t offset_ms : cooldown_checks_ms) {
        REQUIRE(!clock_network_retry_cooldown_is_expired(
            &state, burst_end_ms + offset_ms));
        REQUIRE_EQ(state.status, CLOCK_NETWORK_OFFLINE);
        require_color(state.status, 0, 255, 0, 0, 3);
    }

    const uint64_t cooldown_end_ms =
        burst_end_ms + CLOCK_NETWORK_COOLDOWN_MS;
    REQUIRE(clock_network_retry_cooldown_is_expired(&state,
                                                     cooldown_end_ms));
    clock_network_retry_start_burst(&state, cooldown_end_ms);
    REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
    REQUIRE(clock_network_retry_attempt_is_due(&state, cooldown_end_ms));
    REQUIRE_EQ(clock_network_retry_begin_attempt(&state), 1);
    require_color(state.status, 0, 255, 255, 255, 3);
}

void test_ethernet_and_wifi_reconnect_bursts()
{
    require_reconnect_burst_and_cooldown_timeline(
        CLOCK_NETWORK_MODE_ETHERNET,
        CLOCK_NETWORK_INTERFACE_ETHERNET);
    require_reconnect_burst_and_cooldown_timeline(
        CLOCK_NETWORK_MODE_WIFI,
        CLOCK_NETWORK_INTERFACE_WIFI);
}

void require_success_during_each_attempt_resets_burst(
    clock_network_mode_t mode,
    clock_network_interface_t expected_interface)
{
    REQUIRE_EQ(clock_network_selected_interface(mode), expected_interface);
    for (uint8_t success_attempt = 1;
         success_attempt <= CLOCK_NETWORK_ATTEMPTS_PER_BURST;
         ++success_attempt) {
        clock_network_retry_state_t state = {};
        const uint64_t burst_start_ms =
            100000ULL * static_cast<uint64_t>(success_attempt);
        clock_network_retry_state_init(&state, burst_start_ms);

        for (uint8_t attempt = 1; attempt <= success_attempt; ++attempt) {
            const uint64_t attempt_ms = state.next_action_ms;
            const uint64_t expected_attempt_ms =
                burst_start_ms +
                static_cast<uint64_t>(attempt - 1U) *
                    CLOCK_NETWORK_RETRY_SLOT_MS;
            REQUIRE_EQ(attempt_ms, expected_attempt_ms);
            REQUIRE(clock_network_retry_attempt_is_due(&state, attempt_ms));
            REQUIRE_EQ(clock_network_retry_begin_attempt(&state), attempt);

            if (attempt == success_attempt) {
                const uint64_t success_ms = attempt_ms + 4000ULL;
                REQUIRE(!clock_network_retry_burst_is_exhausted(&state,
                                                                 success_ms));
                clock_network_retry_connected(&state);
            } else {
                clock_network_retry_attempt_failed(&state);
            }
        }

        REQUIRE_EQ(state.status, CLOCK_NETWORK_ONLINE);
        REQUIRE_EQ(state.attempt, 0);
        REQUIRE(state.attempt_complete);
        REQUIRE_EQ(state.burst_started_ms, 0);
        REQUIRE_EQ(state.next_action_ms, 0);
        require_color(state.status, 0, 0, 255, 0, 3);

        clock_network_retry_attempt_failed(&state);
        REQUIRE_EQ(state.status, CLOCK_NETWORK_ONLINE);
        REQUIRE_EQ(state.attempt, 0);
    }
}

void test_ethernet_and_wifi_success_resets_burst()
{
    require_success_during_each_attempt_resets_burst(
        CLOCK_NETWORK_MODE_ETHERNET,
        CLOCK_NETWORK_INTERFACE_ETHERNET);
    require_success_during_each_attempt_resets_burst(
        CLOCK_NETWORK_MODE_WIFI,
        CLOCK_NETWORK_INTERFACE_WIFI);
}

void test_duplicate_failures_do_not_consume_attempts()
{
    const uint8_t attempts_with_duplicate_events[] = {1, 2, 3};

    for (uint8_t duplicate_attempt : attempts_with_duplicate_events) {
        clock_network_retry_state_t state = {};
        const uint64_t burst_start_ms =
            1000ULL * static_cast<uint64_t>(duplicate_attempt);
        clock_network_retry_state_init(&state, burst_start_ms);

        for (uint8_t attempt = 1; attempt < duplicate_attempt; ++attempt) {
            begin_and_fail_attempt(&state, state.next_action_ms, attempt);
        }

        REQUIRE(clock_network_retry_attempt_is_due(&state,
                                                    state.next_action_ms));
        REQUIRE_EQ(clock_network_retry_begin_attempt(&state),
                   duplicate_attempt);
        REQUIRE(!state.attempt_complete);

        clock_network_retry_attempt_failed(&state);
        clock_network_retry_attempt_failed(&state);
        clock_network_retry_attempt_failed(&state);
        REQUIRE_EQ(state.attempt, duplicate_attempt);
        REQUIRE(state.attempt_complete);
        REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);

        if (duplicate_attempt < CLOCK_NETWORK_ATTEMPTS_PER_BURST) {
            REQUIRE(!clock_network_retry_attempt_is_due(
                &state, state.next_action_ms - 1));
            REQUIRE(clock_network_retry_attempt_is_due(
                &state, state.next_action_ms));
        } else {
            REQUIRE(!clock_network_retry_attempt_is_due(
                &state, state.next_action_ms));
            REQUIRE(clock_network_retry_burst_is_exhausted(
                &state, state.next_action_ms));
        }
    }
}

void test_reconnect_bursts_repeat_indefinitely()
{
    clock_network_retry_state_t state = {};
    uint64_t burst_start_ms = 0;
    clock_network_retry_state_init(&state, burst_start_ms);

    for (int cycle = 0; cycle < 3; ++cycle) {
        for (uint8_t attempt = 1;
             attempt < CLOCK_NETWORK_ATTEMPTS_PER_BURST;
             ++attempt) {
            begin_and_fail_attempt(&state, state.next_action_ms, attempt);
            REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
        }

        const uint64_t final_attempt_start_ms =
            burst_start_ms + CLOCK_NETWORK_FINAL_ATTEMPT_START_MS;
        REQUIRE_EQ(state.next_action_ms, final_attempt_start_ms);
        REQUIRE(clock_network_retry_attempt_is_due(
            &state, final_attempt_start_ms));
        REQUIRE_EQ(clock_network_retry_begin_attempt(&state), 3);
        REQUIRE(!state.attempt_complete);
        REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);

        const uint64_t final_failure_ms =
            burst_start_ms + CLOCK_NETWORK_BURST_DURATION_MS;
        REQUIRE(!clock_network_retry_burst_is_exhausted(
            &state, final_failure_ms - 1));
        clock_network_retry_attempt_failed(&state);
        REQUIRE(clock_network_retry_burst_is_exhausted(&state,
                                                        final_failure_ms));
        clock_network_retry_enter_cooldown(&state, final_failure_ms);
        REQUIRE_EQ(state.status, CLOCK_NETWORK_OFFLINE);

        const uint64_t cooldown_end_ms =
            final_failure_ms + CLOCK_NETWORK_COOLDOWN_MS;
        REQUIRE(!clock_network_retry_cooldown_is_expired(
            &state, cooldown_end_ms - 1));
        REQUIRE(clock_network_retry_cooldown_is_expired(
            &state, cooldown_end_ms));

        burst_start_ms = cooldown_end_ms;
        clock_network_retry_start_burst(&state, burst_start_ms);
        REQUIRE_EQ(state.status, CLOCK_NETWORK_CONNECTING);
    }
}

void require_color(clock_network_status_t status,
                   int64_t now_us,
                   uint8_t red,
                   uint8_t green,
                   uint8_t blue,
                   size_t expected_count)
{
    clock_network_indicator_pixel_t pixels[CLOCK_NETWORK_INDICATOR_PIXEL_COUNT] = {};
    const size_t count = clock_network_indicator_build_pixels(
        64,
        32,
        status,
        now_us,
        pixels,
        CLOCK_NETWORK_INDICATOR_PIXEL_COUNT);
    REQUIRE_EQ(count, expected_count);

    for (size_t index = 0; index < count; ++index) {
        REQUIRE_EQ(pixels[index].red, red);
        REQUIRE_EQ(pixels[index].green, green);
        REQUIRE_EQ(pixels[index].blue, blue);
    }
}

void test_upper_right_coordinates_and_colors()
{
    clock_network_indicator_pixel_t pixels[CLOCK_NETWORK_INDICATOR_PIXEL_COUNT] = {};
    const size_t count = clock_network_indicator_build_pixels(
        64,
        32,
        CLOCK_NETWORK_ONLINE,
        0,
        pixels,
        CLOCK_NETWORK_INDICATOR_PIXEL_COUNT);
    REQUIRE_EQ(count, 3);

    const uint16_t expected[3][2] = {
        {63, 0}, {62, 0}, {63, 1},
    };

    for (size_t index = 0; index < count; ++index) {
        REQUIRE_EQ(pixels[index].x, expected[index][0]);
        REQUIRE_EQ(pixels[index].y, expected[index][1]);
        for (size_t other = index + 1; other < count; ++other) {
            REQUIRE(pixels[index].x != pixels[other].x ||
                    pixels[index].y != pixels[other].y);
        }
    }

    require_color(CLOCK_NETWORK_ONLINE, 0, 0, 255, 0, 3);
    require_color(CLOCK_NETWORK_OFFLINE, 0, 255, 0, 0, 3);
    require_color(CLOCK_NETWORK_CONNECTING, 0, 255, 255, 255, 3);
    require_color(CLOCK_NETWORK_CONNECTING, 500000, 0, 0, 0, 0);

    REQUIRE_EQ(clock_network_indicator_build_pixels(
                   1,
                   32,
                   CLOCK_NETWORK_ONLINE,
                   0,
                   pixels,
                   CLOCK_NETWORK_INDICATOR_PIXEL_COUNT),
               0);
    REQUIRE_EQ(clock_network_indicator_build_pixels(
                   64,
                   1,
                   CLOCK_NETWORK_ONLINE,
                   0,
                   pixels,
                   CLOCK_NETWORK_INDICATOR_PIXEL_COUNT),
               0);
}

void test_connecting_blink_boundaries()
{
    REQUIRE(clock_network_indicator_is_visible(CLOCK_NETWORK_CONNECTING, 0));
    REQUIRE(clock_network_indicator_is_visible(CLOCK_NETWORK_CONNECTING, 499000));
    REQUIRE(!clock_network_indicator_is_visible(CLOCK_NETWORK_CONNECTING, 500000));
    REQUIRE(!clock_network_indicator_is_visible(CLOCK_NETWORK_CONNECTING, 999000));
    REQUIRE(clock_network_indicator_is_visible(CLOCK_NETWORK_CONNECTING, 1000000));
}

}  // namespace

int main()
{
    test_ethernet_and_wifi_reconnect_bursts();
    test_ethernet_and_wifi_success_resets_burst();
    test_duplicate_failures_do_not_consume_attempts();
    test_reconnect_bursts_repeat_indefinitely();
    test_upper_right_coordinates_and_colors();
    test_connecting_blink_boundaries();
    std::puts("network status tests passed");
    return 0;
}
