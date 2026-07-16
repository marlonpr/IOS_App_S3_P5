#include <cstdio>
#include <cstdlib>

#include "clock_buttons.h"
#include "clock_button_logic.h"

#define REQUIRE(expression)                                                   \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

static uint8_t update(clock_button_state_t *state,
                      int level,
                      uint32_t ms,
                      bool is_up = false)
{
    return clock_button_state_update(state, level, ms, 1000, is_up ? 10000 : 0);
}

static void test_explicit_slots_do_not_use_enum_as_index()
{
    static const button_t kButtons[] = {BTN_MENU, BTN_UP, BTN_DOWN};
    clock_button_state_t states[3] = {};
    int levels[3] = {1, 1, 0};

    REQUIRE(kButtons[0] == BTN_MENU);
    REQUIRE(kButtons[1] == BTN_UP);
    REQUIRE(kButtons[2] == BTN_DOWN);

    uint8_t down_events = CLOCK_BUTTON_EVENT_NONE;
    for (size_t slot = 0; slot < 3; ++slot) {
        const button_t btn = kButtons[slot];
        const uint8_t events = update(&states[slot], levels[slot], 100,
                                      btn == BTN_UP);
        if (btn == BTN_DOWN) {
            down_events = events;
        }
    }

    REQUIRE(down_events == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(!states[0].pressed);
    REQUIRE(!states[1].pressed);
    REQUIRE(states[2].pressed);
}

static void test_down_hold_and_release_reset()
{
    clock_button_state_t down = {};
    REQUIRE(update(&down, 0, 100) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&down, 0, 1099) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&down, 0, 1100) == CLOCK_BUTTON_EVENT_NORMAL_ACTION);
    REQUIRE(update(&down, 0, 5000) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&down, 1, 5001) == CLOCK_BUTTON_EVENT_RELEASE);
    REQUIRE(update(&down, 0, 6000) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&down, 0, 7000) == CLOCK_BUTTON_EVENT_NORMAL_ACTION);
}

static void test_up_does_not_consume_down()
{
    clock_button_state_t up = {};
    clock_button_state_t down = {};
    REQUIRE(update(&up, 0, 0, true) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&down, 0, 500) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&down, 0, 1500) == CLOCK_BUTTON_EVENT_NORMAL_ACTION);
    REQUIRE(update(&up, 0, 9999, true) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&up, 0, 10000, true) == CLOCK_BUTTON_EVENT_LONG_ACTION);
    REQUIRE(update(&down, 1, 10001) == CLOCK_BUTTON_EVENT_RELEASE);
    REQUIRE(update(&up, 1, 10001, true) == CLOCK_BUTTON_EVENT_RELEASE);
}

static void test_up_normal_action_is_resolved_on_release()
{
    clock_button_state_t up = {};
    REQUIRE(update(&up, 0, 100, true) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&up, 0, 1100, true) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&up, 1, 1101, true) ==
            (CLOCK_BUTTON_EVENT_RELEASE | CLOCK_BUTTON_EVENT_NORMAL_ACTION));
}

int main()
{
    test_explicit_slots_do_not_use_enum_as_index();
    test_down_hold_and_release_reset();
    test_up_does_not_consume_down();
    test_up_normal_action_is_resolved_on_release();
    std::puts("button logic tests passed");
    return 0;
}
