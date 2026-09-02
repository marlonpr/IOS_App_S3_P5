#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clock_network.h"
#include "clock_settings.h"

void network_settings_test_reset(void);
void network_settings_test_store_raw(unsigned char value);
unsigned char network_settings_test_read_raw(void);
int network_settings_test_network_mode_write_count(void);

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
#define REQUIRE_TEXT(actual, expected) \
    REQUIRE(std::strcmp((actual), (expected)) == 0)

namespace {

int s_save_count = 0;
uint8_t s_last_saved_mode = 0xff;

esp_err_t fake_save(uint8_t mode, void *)
{
    ++s_save_count;
    s_last_saved_mode = mode;
    return ESP_OK;
}

void test_only_ethernet_and_wifi_are_selectable()
{
    REQUIRE_EQ(static_cast<uint8_t>(CLOCK_NETWORK_MODE_ETHERNET), 1U);
    REQUIRE_EQ(static_cast<uint8_t>(CLOCK_NETWORK_MODE_WIFI), 2U);
    REQUIRE(!clock_network_mode_is_valid(
        CLOCK_NETWORK_MODE_LEGACY_AUTO_VALUE));
    REQUIRE(clock_network_mode_is_valid(CLOCK_NETWORK_MODE_ETHERNET));
    REQUIRE(clock_network_mode_is_valid(CLOCK_NETWORK_MODE_WIFI));
    REQUIRE(!clock_network_mode_is_valid(3));
}

void test_nvs_defaults_and_supported_values()
{
    network_settings_test_reset();
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(network_settings_test_network_mode_write_count(), 0);

    REQUIRE_EQ(clock_settings_save_network_mode(CLOCK_NETWORK_MODE_ETHERNET),
               ESP_OK);
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_ETHERNET);
    REQUIRE_EQ(clock_settings_save_network_mode(CLOCK_NETWORK_MODE_WIFI),
               ESP_OK);
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(clock_settings_save_network_mode(
                   CLOCK_NETWORK_MODE_LEGACY_AUTO_VALUE),
               ESP_ERR_INVALID_ARG);
    REQUIRE_EQ(clock_settings_save_network_mode(3), ESP_ERR_INVALID_ARG);
}

void test_legacy_auto_migrates_to_wifi_once()
{
    network_settings_test_reset();
    network_settings_test_store_raw(CLOCK_NETWORK_MODE_LEGACY_AUTO_VALUE);

    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(network_settings_test_read_raw(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(network_settings_test_network_mode_write_count(), 1);

    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(network_settings_test_network_mode_write_count(), 1);
}

void test_unknown_mode_falls_back_and_is_repaired()
{
    network_settings_test_reset();
    network_settings_test_store_raw(99);

    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(network_settings_test_read_raw(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(network_settings_test_network_mode_write_count(), 1);
}

void require_cycle(clock_network_mode_t current,
                   clock_network_mode_t expected)
{
    s_save_count = 0;
    s_last_saved_mode = 0xff;
    clock_network_mode_t next = current;
    REQUIRE_EQ(clock_network_cycle_saved_mode(current,
                                              fake_save,
                                              nullptr,
                                              &next),
               ESP_OK);
    REQUIRE_EQ(next, expected);
    REQUIRE_EQ(s_save_count, 1);
    REQUIRE_EQ(s_last_saved_mode, static_cast<uint8_t>(expected));
}

void test_saved_mode_cycle_has_no_live_interface_side_effect()
{
    require_cycle(CLOCK_NETWORK_MODE_ETHERNET, CLOCK_NETWORK_MODE_WIFI);
    require_cycle(CLOCK_NETWORK_MODE_WIFI, CLOCK_NETWORK_MODE_ETHERNET);
}

void test_labels()
{
    REQUIRE_TEXT(clock_network_mode_label(CLOCK_NETWORK_MODE_ETHERNET),
                 "Ethernet");
    REQUIRE_TEXT(clock_network_mode_label(CLOCK_NETWORK_MODE_WIFI), "Wi-Fi");
    REQUIRE_TEXT(clock_network_mode_panel_label(CLOCK_NETWORK_MODE_ETHERNET),
                 "NET: ETH");
    REQUIRE_TEXT(clock_network_mode_panel_label(CLOCK_NETWORK_MODE_WIFI),
                 "NET: WIFI");
    REQUIRE_TEXT(clock_network_result_message(CLOCK_NETWORK_INTERFACE_ETHERNET),
                 "ETH CONNECTED");
    REQUIRE_TEXT(clock_network_result_message(CLOCK_NETWORK_INTERFACE_WIFI),
                 "WIFI CONNECTED");
    REQUIRE_TEXT(clock_network_result_message(CLOCK_NETWORK_INTERFACE_NONE),
                 "NET NOT CONNECTED");
    REQUIRE(clock_network_is_result_message("ETH CONNECTED"));
    REQUIRE(clock_network_is_result_message("WIFI CONNECTED"));
    REQUIRE(clock_network_is_result_message("NET NOT CONNECTED"));
    REQUIRE(!clock_network_is_result_message("REBOOT TO APPLY"));
}

void require_result_waits_for_startup(clock_network_interface_t interface)
{
    clock_network_result_gate_t gate = {};
    clock_network_result_gate_init(&gate);

    const char *expected = clock_network_result_message(interface);
    REQUIRE(!clock_network_result_gate_submit(&gate, expected, 3000));

    char released[CLOCK_NETWORK_RESULT_MESSAGE_CAPACITY] = {};
    uint32_t duration_ms = 0;
    REQUIRE(clock_network_result_gate_finish_startup(&gate,
                                                     released,
                                                     sizeof(released),
                                                     &duration_ms));
    REQUIRE_TEXT(released, expected);
    REQUIRE_EQ(duration_ms, 3000U);
}

void test_network_results_wait_for_startup()
{
    require_result_waits_for_startup(CLOCK_NETWORK_INTERFACE_ETHERNET);
    require_result_waits_for_startup(CLOCK_NETWORK_INTERFACE_WIFI);
    require_result_waits_for_startup(CLOCK_NETWORK_INTERFACE_NONE);
}

void test_result_is_immediate_after_startup()
{
    clock_network_result_gate_t gate = {};
    clock_network_result_gate_init(&gate);

    char released[CLOCK_NETWORK_RESULT_MESSAGE_CAPACITY] = {};
    uint32_t duration_ms = 0;
    REQUIRE(!clock_network_result_gate_finish_startup(&gate,
                                                      released,
                                                      sizeof(released),
                                                      &duration_ms));
    REQUIRE(clock_network_result_gate_submit(&gate, "WIFI CONNECTED", 3000));
}

void test_non_result_messages_are_never_delayed()
{
    clock_network_result_gate_t gate = {};
    clock_network_result_gate_init(&gate);

    REQUIRE(clock_network_result_gate_submit(&gate, "REBOOT TO APPLY", 2500));
}

void test_each_mode_selects_only_its_interface()
{
    REQUIRE_EQ(clock_network_selected_interface(CLOCK_NETWORK_MODE_ETHERNET),
               CLOCK_NETWORK_INTERFACE_ETHERNET);
    REQUIRE_EQ(clock_network_selected_interface(CLOCK_NETWORK_MODE_WIFI),
               CLOCK_NETWORK_INTERFACE_WIFI);
}

void test_long_hold_fires_once_per_hold()
{
    clock_network_long_hold_t hold = {};
    REQUIRE(!clock_network_long_hold_update(&hold, true, 9999));
    REQUIRE(clock_network_long_hold_update(&hold, true, 10000));
    REQUIRE(!clock_network_long_hold_update(&hold, true, 15000));
    REQUIRE(!clock_network_long_hold_update(&hold, false, 0));
    REQUIRE(clock_network_long_hold_update(&hold, true, 10000));
}

void test_services_and_ota_start_once_after_ip()
{
    clock_network_service_guard_t guard = {};
    int tcp_starts = 0;
    int logo_starts = 0;
    int mdns_starts = 0;
    int ota_starts = 0;

    auto on_ip_state = [&](bool has_ip) {
        if (!has_ip || !clock_network_service_guard_try_start(&guard)) {
            return;
        }
        ++tcp_starts;
        ++logo_starts;
        ++mdns_starts;
        ++ota_starts;
    };

    on_ip_state(false);
    REQUIRE_EQ(tcp_starts, 0);
    REQUIRE_EQ(ota_starts, 0);
    on_ip_state(true);
    on_ip_state(true);
    REQUIRE_EQ(tcp_starts, 1);
    REQUIRE_EQ(logo_starts, 1);
    REQUIRE_EQ(mdns_starts, 1);
    REQUIRE_EQ(ota_starts, 1);
}

}  // namespace

int main()
{
    test_only_ethernet_and_wifi_are_selectable();
    test_nvs_defaults_and_supported_values();
    test_legacy_auto_migrates_to_wifi_once();
    test_unknown_mode_falls_back_and_is_repaired();
    test_saved_mode_cycle_has_no_live_interface_side_effect();
    test_labels();
    test_network_results_wait_for_startup();
    test_result_is_immediate_after_startup();
    test_non_result_messages_are_never_delayed();
    test_each_mode_selects_only_its_interface();
    test_long_hold_fires_once_per_hold();
    test_services_and_ota_start_once_after_ip();
    std::puts("network mode tests passed");
    return 0;
}
