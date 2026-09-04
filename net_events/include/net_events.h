#ifndef NET_EVENTS_H
#define NET_EVENTS_H

#include <stdint.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Network link state, as events.
 *
 * This exists so that the things which care about connectivity -- mDNS, SNTP,
 * MQTT, a web server -- do not have to be known to the thing that provides it.
 * Previously the WiFi manager called mDNS and SNTP directly from its event
 * handler, which meant WiFi could not be built without both of them and neither
 * could be used without WiFi. The dependency pointed the wrong way: upward, from
 * the link layer to the services running over it.
 *
 * Now the link layer posts and the services subscribe, so the graph only points
 * down. It also means the link need not be WiFi: an Ethernet driver, or a
 * cellular modem, posts the same events and every subscriber works unchanged.
 *
 * Events are posted to the default event loop, so `esp_event_loop_create_default()`
 * must have been called first.
 */

ESP_EVENT_DECLARE_BASE(NET_EVENT);

typedef enum {
    /* The device has an address and can reach the network. Data:
     * net_event_link_t. Posted again on a reconnect, so a handler must be
     * prepared to run more than once. */
    NET_EVENT_LINK_UP,

    /* The address is gone. Data: none. A service holding sockets should drop
     * them here rather than waiting for them to time out. */
    NET_EVENT_LINK_DOWN,

    /* The device is serving its own access point, having found no known network.
     * Data: net_event_link_t. Clients can reach it, but it has no route to the
     * internet -- which is why this is distinct from LINK_UP: advertising over
     * mDNS makes sense here, synchronising a clock does not. */
    NET_EVENT_AP_STARTED,

    /* The access point has been taken down. Data: none. */
    NET_EVENT_AP_STOPPED,

    /* The system clock has been set from an external source. Data: struct
     * timeval, the newly set time. Anything that timestamps records wants this:
     * before it, the clock is whatever the last boot left behind. */
    NET_EVENT_TIME_SYNCED,
} net_event_id_t;

typedef struct {
    /* Dotted-quad address, NUL-terminated. Fixed size because it travels through
     * the event loop by value, and an event payload must not carry a pointer to
     * something the poster may free. */
    char ip[16];
} net_event_link_t;

/*
 * Post to the default event loop. `data` may be NULL for events that carry none.
 *
 * A thin wrapper over esp_event_post so posters need not repeat the base and the
 * timeout, and so the timeout is decided in one place: it does not block, because
 * these are posted from event handlers and timer callbacks where blocking would
 * stall the loop that is trying to deliver them.
 */
esp_err_t net_events_post(net_event_id_t id, const void *data, size_t data_len);

/* Human-readable event name, for logging. Never NULL. */
const char *net_events_name(net_event_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* NET_EVENTS_H */
