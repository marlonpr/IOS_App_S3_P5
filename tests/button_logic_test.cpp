#include <cstdio>
#include <cstdlib>

#include "clock_buttons.h"
#include "clock_button_logic.h"
#include "clock_menu_model.h"

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
                      uint32_t normal_hold_ms = 1000,
                      uint32_t long_hold_ms = 0)
{
    return clock_button_state_update(state,
                                     level,
                                     ms,
                                     normal_hold_ms,
                                     long_hold_ms);
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
        const uint8_t events = update(&states[slot], levels[slot], 100);
        if (btn == BTN_DOWN) {
            down_events = events;
        }
    }

    REQUIRE(down_events == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(!states[0].pressed);
    REQUIRE(!states[1].pressed);
    REQUIRE(states[2].pressed);
}

static void test_up_hold_is_normal_display_mode_action()
{
    clock_button_state_t up = {};
    REQUIRE(update(&up, 0, 100) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&up, 0, 1099) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&up, 0, 1100) == CLOCK_BUTTON_EVENT_NORMAL_ACTION);
    REQUIRE(update(&up, 0, 10100) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&up, 1, 10101) == CLOCK_BUTTON_EVENT_RELEASE);
}

static void test_down_ten_second_hold_is_one_shot_long_action()
{
    clock_button_state_t down = {};
    REQUIRE(update(&down, 0, 0, 0, 10000) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&down, 0, 9999, 0, 10000) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&down, 0, 10000, 0, 10000) == CLOCK_BUTTON_EVENT_LONG_ACTION);
    REQUIRE(update(&down, 0, 15000, 0, 10000) == CLOCK_BUTTON_EVENT_NONE);
    REQUIRE(update(&down, 1, 15001, 0, 10000) == CLOCK_BUTTON_EVENT_RELEASE);
}

static void test_down_short_press_is_available_to_menu_only()
{
    clock_button_state_t down = {};
    REQUIRE(update(&down, 0, 100, 0, 10000) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&down, 1, 200, 0, 10000) == CLOCK_BUTTON_EVENT_RELEASE);
}

static void test_up_and_down_states_are_independent()
{
    clock_button_state_t up = {};
    clock_button_state_t down = {};
    REQUIRE(update(&down, 0, 0, 0, 10000) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&up, 0, 500) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&up, 0, 1500) == CLOCK_BUTTON_EVENT_NORMAL_ACTION);
    REQUIRE(update(&down, 0, 10000, 0, 10000) == CLOCK_BUTTON_EVENT_LONG_ACTION);
    REQUIRE(update(&up, 1, 10001) == CLOCK_BUTTON_EVENT_RELEASE);
    REQUIRE(update(&down, 1, 10001, 0, 10000) == CLOCK_BUTTON_EVENT_RELEASE);

    REQUIRE(update(&down, 0, 11000, 0, 10000) == CLOCK_BUTTON_EVENT_PRESS);
    REQUIRE(update(&down, 1, 11001, 0, 10000) == CLOCK_BUTTON_EVENT_RELEASE);
}

static void test_format_is_reachable_from_menu()
{
    REQUIRE(clock_menu_next_field(CLOCK_MENU_FIELD_BRIGHTNESS) ==
            CLOCK_MENU_FIELD_FORMAT);
    REQUIRE(clock_menu_next_field(CLOCK_MENU_FIELD_FORMAT) ==
            CLOCK_MENU_FIELD_HOUR);
    REQUIRE(clock_menu_toggle_format(0) == 1);
    REQUIRE(clock_menu_toggle_format(1) == 0);
}

int main()
{
    test_explicit_slots_do_not_use_enum_as_index();
    test_up_hold_is_normal_display_mode_action();
    test_down_ten_second_hold_is_one_shot_long_action();
    test_down_short_press_is_available_to_menu_only();
    test_up_and_down_states_are_independent();
    test_format_is_reachable_from_menu();
    std::puts("button logic tests passed");
    return 0;
}
