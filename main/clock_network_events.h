#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOCK_NETWORK_INTERFACE_EVENT_STARTED = 0,
    CLOCK_NETWORK_INTERFACE_EVENT_LINK_UP,
    CLOCK_NETWORK_INTERFACE_EVENT_DISCONNECTED,
    CLOCK_NETWORK_INTERFACE_EVENT_GOT_IP,
    CLOCK_NETWORK_INTERFACE_EVENT_LOST_IP,
    CLOCK_NETWORK_INTERFACE_EVENT_STOPPED,
} clock_network_interface_event_t;

typedef void (*clock_network_interface_event_callback_t)(
    clock_network_interface_event_t event,
    uint32_t detail,
    void *context);

#ifdef __cplusplus
}
#endif
