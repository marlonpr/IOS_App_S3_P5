#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*clock_ethernet_rx_callback_t)(
    const uint8_t *rx,
    int rx_len,
    uint8_t *tx,
    int tx_max
);

/*
 * Initialize W5500 Ethernet using static IP.
 */
esp_err_t clock_ethernet_init_static(void);

/*
 * Initialize W5500 Ethernet using DHCP.
 * You can use this later when connected to a router.
 */
esp_err_t clock_ethernet_init_dhcp(void);

/* Wait until the W5500 interface has received an IP address. */
bool clock_ethernet_wait_for_ip(uint32_t timeout_ms);

/* True when IP, gateway, and DNS are available for routed OTA traffic. */
bool clock_ethernet_route_is_ready(void);

/* Stop the W5500 driver after a bounded boot attempt. */
esp_err_t clock_ethernet_stop(void);

/*
 * Start the existing interface-independent TCP server.
 * The callback is called once for each complete protocol frame.
 */
esp_err_t clock_ethernet_start_tcp_server(clock_ethernet_rx_callback_t rx_callback);

#ifdef __cplusplus
}
#endif
