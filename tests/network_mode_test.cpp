#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clock_network.h"
#include "clock_settings.h"

void network_settings_test_reset(void);
void network_settings_test_store_raw(unsigned char value);

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
#define REQUIRE_TEXT(actual, expected) REQUIRE(std::strcmp((actual), (expected)) == 0)

namespace {

int s_save_count = 0;
int s_interface_start_count = 0;
uint8_t s_last_saved_mode = 0xff;

esp_err_t fake_save(uint8_t mode, void *)
{
    ++s_save_count;
    s_last_saved_mode = mode;
    return ESP_OK;
}

void reset_cycle_spies()
{
    s_save_count = 0;
    s_interface_start_count = 0;
    s_last_saved_mode = 0xff;
}

void test_nvs_defaults_and_values()
{
    network_settings_test_reset();
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_AUTO);

    network_settings_test_store_raw(99);
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_AUTO);

    REQUIRE_EQ(clock_settings_save_network_mode(CLOCK_NETWORK_MODE_AUTO), ESP_OK);
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_AUTO);
    REQUIRE_EQ(clock_settings_save_network_mode(CLOCK_NETWORK_MODE_ETHERNET), ESP_OK);
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_ETHERNET);
    REQUIRE_EQ(clock_settings_save_network_mode(CLOCK_NETWORK_MODE_WIFI), ESP_OK);
    REQUIRE_EQ(clock_settings_load_network_mode(), CLOCK_NETWORK_MODE_WIFI);
    REQUIRE_EQ(clock_settings_save_network_mode(3), ESP_ERR_INVALID_ARG);
}

void require_cycle(clock_network_mode_t current,
                   clock_network_mode_t expected)
{
    reset_cycle_spies();
    clock_network_mode_t next = current;
    REQUIRE_EQ(clock_network_cycle_saved_mode(current,
                                              fake_save,
                                              nullptr,
                                              &next),
               ESP_OK);
    REQUIRE_EQ(next, expected);
    REQUIRE_EQ(s_save_count, 1);
    REQUIRE_EQ(s_last_saved_mode, static_cast<uint8_t>(expected));
    REQUIRE_EQ(s_interface_start_count, 0);
}

void test_saved_mode_cycle_has_no_live_interface_side_effect()
{
    require_cycle(CLOCK_NETWORK_MODE_AUTO, CLOCK_NETWORK_MODE_ETHERNET);
    require_cycle(CLOCK_NETWORK_MODE_ETHERNET, CLOCK_NETWORK_MODE_WIFI);
    require_cycle(CLOCK_NETWORK_MODE_WIFI, CLOCK_NETWORK_MODE_AUTO);
}

void test_labels()
{
    REQUIRE_TEXT(clock_network_mode_label(CLOCK_NETWORK_MODE_AUTO), "Auto");
    REQUIRE_TEXT(clock_network_mode_label(CLOCK_NETWORK_MODE_ETHERNET), "Ethernet");
    REQUIRE_TEXT(clock_network_mode_label(CLOCK_NETWORK_MODE_WIFI), "Wi-Fi");
    REQUIRE_TEXT(clock_network_mode_panel_label(CLOCK_NETWORK_MODE_AUTO), "NET: AUTO");
    REQUIRE_TEXT(clock_network_mode_panel_label(CLOCK_NETWORK_MODE_ETHERNET), "NET: ETH");
    REQUIRE_TEXT(clock_network_mode_panel_label(CLOCK_NETWORK_MODE_WIFI), "NET: WIFI");
    REQUIRE_TEXT(clock_network_result_message(CLOCK_NETWORK_INTERFACE_ETHERNET),
                 "ETH CONNECTED");
    REQUIRE_TEXT(clock_network_result_message(CLOCK_NETWORK_INTERFACE_WIFI),
                 "WIFI CONNECTED");
    REQUIRE_TEXT(clock_network_result_message(CLOCK_NETWORK_INTERFACE_NONE),
                 "NET NOT CONNECTED");
}

void test_boot_order()
{
    REQUIRE_EQ(clock_network_first_interface(CLOCK_NETWORK_MODE_AUTO),
               CLOCK_NETWORK_INTERFACE_ETHERNET);
    REQUIRE_EQ(clock_network_fallback_interface(
                   CLOCK_NETWORK_MODE_AUTO,
                   CLOCK_NETWORK_INTERFACE_ETHERNET),
               CLOCK_NETWORK_INTERFACE_WIFI);
    REQUIRE_EQ(clock_network_first_interface(CLOCK_NETWORK_MODE_ETHERNET),
               CLOCK_NETWORK_INTERFACE_ETHERNET);
    REQUIRE_EQ(clock_network_fallback_interface(
                   CLOCK_NETWORK_MODE_ETHERNET,
                   CLOCK_NETWORK_INTERFACE_ETHERNET),
               CLOCK_NETWORK_INTERFACE_NONE);
    REQUIRE_EQ(clock_network_first_interface(CLOCK_NETWORK_MODE_WIFI),
               CLOCK_NETWORK_INTERFACE_WIFI);
    REQUIRE_EQ(clock_network_fallback_interface(
                   CLOCK_NETWORK_MODE_WIFI,
                   CLOCK_NETWORK_INTERFACE_WIFI),
               CLOCK_NETWORK_INTERFACE_NONE);
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
    test_nvs_defaults_and_values();
    test_saved_mode_cycle_has_no_live_interface_side_effect();
    test_labels();
    test_boot_order();
    test_long_hold_fires_once_per_hold();
    test_services_and_ota_start_once_after_ip();
    std::puts("network mode tests passed");
    return 0;
}
