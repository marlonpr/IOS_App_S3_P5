#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "clock_ethernet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_NETWORK_MODE_LEGACY_AUTO_VALUE 0U

typedef enum {
    CLOCK_NETWORK_MODE_ETHERNET = 1,
    CLOCK_NETWORK_MODE_WIFI = 2,
} clock_network_mode_t;

typedef enum {
    CLOCK_NETWORK_INTERFACE_NONE = 0,
    CLOCK_NETWORK_INTERFACE_ETHERNET = 1,
    CLOCK_NETWORK_INTERFACE_WIFI = 2,
} clock_network_interface_t;

typedef enum {
    CLOCK_NETWORK_OFFLINE = 0,
    CLOCK_NETWORK_CONNECTING = 1,
    CLOCK_NETWORK_ONLINE = 2,
} clock_network_status_t;

#define CLOCK_NETWORK_RETRY_SLOT_MS (20UL * 1000UL)
#define CLOCK_NETWORK_ATTEMPTS_PER_BURST 3U
#define CLOCK_NETWORK_COOLDOWN_MS (5UL * 60UL * 1000UL)
#define CLOCK_NETWORK_FINAL_ATTEMPT_START_MS                         \
    (CLOCK_NETWORK_RETRY_SLOT_MS *                                  \
     (CLOCK_NETWORK_ATTEMPTS_PER_BURST - 1U))
#define CLOCK_NETWORK_BURST_DURATION_MS                              \
    (CLOCK_NETWORK_RETRY_SLOT_MS * CLOCK_NETWORK_ATTEMPTS_PER_BURST)

typedef struct {
    clock_network_status_t status;
    uint8_t attempt;
    bool attempt_complete;
    uint64_t burst_started_ms;
    uint64_t next_action_ms;
} clock_network_retry_state_t;

typedef struct {
    bool started;
} clock_network_service_guard_t;

typedef struct {
    bool fired;
} clock_network_long_hold_t;

#define CLOCK_NETWORK_RESULT_MESSAGE_CAPACITY 32

typedef struct {
    bool startup_complete;
    bool result_pending;
    char pending_message[CLOCK_NETWORK_RESULT_MESSAGE_CAPACITY];
    uint32_t pending_duration_ms;
} clock_network_result_gate_t;

typedef esp_err_t (*clock_network_save_mode_callback_t)(uint8_t mode,
                                                         void *context);
typedef void (*clock_network_show_message_callback_t)(const char *message,
                                                       uint32_t duration_ms);

bool clock_network_mode_is_valid(uint8_t mode);
clock_network_mode_t clock_network_next_mode(clock_network_mode_t mode);
const char *clock_network_mode_label(clock_network_mode_t mode);
const char *clock_network_mode_panel_label(clock_network_mode_t mode);
const char *clock_network_result_message(clock_network_interface_t interface);
bool clock_network_is_result_message(const char *message);

void clock_network_result_gate_init(clock_network_result_gate_t *gate);
bool clock_network_result_gate_submit(clock_network_result_gate_t *gate,
                                      const char *message,
                                      uint32_t duration_ms);
bool clock_network_result_gate_finish_startup(
    clock_network_result_gate_t *gate,
    char *message,
    size_t message_capacity,
    uint32_t *duration_ms);

clock_network_interface_t clock_network_selected_interface(
    clock_network_mode_t mode);

// IPv4 values are supplied in host byte order (for example 0xC0A8057B
// for 192.168.5.123). The expected network, netmask, and gateway are
// provided by the caller so the Wi-Fi LAN policy can be configured.
bool clock_network_ipv4_matches_policy(uint32_t ip,
                                       uint32_t netmask,
                                       uint32_t gateway,
                                       uint32_t expected_network,
                                       uint32_t expected_netmask,
                                       uint32_t expected_gateway);

bool clock_network_service_guard_try_start(clock_network_service_guard_t *guard);
bool clock_network_long_hold_update(clock_network_long_hold_t *state,
                                    bool pressed,
                                    uint32_t held_ms);

void clock_network_retry_state_init(clock_network_retry_state_t *state,
                                    uint64_t now_ms);
void clock_network_retry_start_burst(clock_network_retry_state_t *state,
                                     uint64_t now_ms);
bool clock_network_retry_attempt_is_due(
    const clock_network_retry_state_t *state,
    uint64_t now_ms);
uint8_t clock_network_retry_begin_attempt(clock_network_retry_state_t *state);
void clock_network_retry_attempt_failed(clock_network_retry_state_t *state);
bool clock_network_retry_burst_is_exhausted(
    const clock_network_retry_state_t *state,
    uint64_t now_ms);
void clock_network_retry_enter_cooldown(clock_network_retry_state_t *state,
                                        uint64_t now_ms);
bool clock_network_retry_cooldown_is_expired(
    const clock_network_retry_state_t *state,
    uint64_t now_ms);
void clock_network_retry_connected(clock_network_retry_state_t *state);
void clock_network_retry_disconnected(clock_network_retry_state_t *state,
                                      uint64_t now_ms);

esp_err_t clock_network_cycle_saved_mode(
    clock_network_mode_t current_mode,
    clock_network_save_mode_callback_t save_callback,
    void *save_context,
    clock_network_mode_t *new_mode);

esp_err_t clock_network_start(clock_network_mode_t boot_mode,
                              clock_ethernet_rx_callback_t rx_callback,
                              clock_network_show_message_callback_t show_message);

clock_network_mode_t clock_network_get_boot_mode(void);
clock_network_interface_t clock_network_get_active_interface(void);
clock_network_status_t clock_network_get_status(void);
bool clock_network_services_started(void);

/* Returns the last accepted local IPv4 address for the active interface. */
bool clock_network_get_local_ipv4(uint32_t *address);
/* Diagnostic IPv4: Wi-Fi returns the latest DHCP lease even while a bad
 * lease is being rejected/retried; Ethernet returns its current address. */
bool clock_network_get_diagnostic_ipv4(uint32_t *address);

#ifdef __cplusplus
}
#endif
