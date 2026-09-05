# net_events

Network link state as events on the default loop.

```c
/* The link layer posts. */
net_events_post(NET_EVENT_LINK_UP, &info, sizeof(info));

/* Everything that needs connectivity subscribes. */
esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, &on_net_event, NULL);
```

This exists so that the things which care about connectivity — mDNS, SNTP, MQTT, a web
server — do not have to be known to the thing that provides it.

In the project this was extracted from, the WiFi manager called mDNS and SNTP directly
from its own event handler, so `networking` had to `REQUIRES mdns_manager ntp_manager`.
That dependency pointed **upward**, from the link layer to the services running over it:
WiFi could not be built without both, and neither could be used without WiFi.

It also means the link need not be WiFi. An Ethernet driver or a cellular modem posting
these events serves every subscriber unchanged.

## Events

| | |
|---|---|
| `NET_EVENT_LINK_UP` | An address, and a route. Data: `net_event_link_t`. Posted again on reconnect, so handlers must tolerate running more than once. |
| `NET_EVENT_LINK_DOWN` | The address is gone. Drop sockets here rather than waiting for them to time out. |
| `NET_EVENT_AP_STARTED` | The device is serving its own access point. Distinct from `LINK_UP` because there are clients but no route: advertising over mDNS makes sense, synchronising a clock does not. |
| `NET_EVENT_AP_STOPPED` | |
| `NET_EVENT_TIME_SYNCED` | The clock has been set. Data: `struct timeval`. Anything timestamping records wants this — before it, the clock holds whatever the last boot left behind. |

`esp_event_loop_create_default()` must have been called first.

Payloads travel by value: an event must not carry a pointer to something the poster may
free. `net_events_post()` never blocks, because posters are event handlers and timer
callbacks where blocking would stall the loop trying to deliver them.
