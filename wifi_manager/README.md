# wifi_manager

WiFi station and access point: scanning, prioritised known networks, connection timeouts,
reconnect backoff, and an optional reachability check.

```c
wifi_manager_config_t cfg;                  /* generated from the schema */
config_store_read(buf, sizeof(buf), &len, NULL);
/* ... parse the "wifi" section into cfg ... */

wifi_manager_init(&cfg);
wifi_manager_start_station_mode();
wifi_manager_scan_and_connect();
```

## It depends on nothing that runs over it

That is the point of this component's rework. State changes are published as
[`net_events`](../net_events/) rather than by calling into the services that care:

```c
net_events_post(NET_EVENT_LINK_UP, &info, sizeof(info));
```

Its predecessor called `mdns_manager_init()` and `esp_netif_sntp_init()` from inside
`wifi_manager_init()`, and `mdns_on_connect()` from its event handler — so `networking`
had to `REQUIRES mdns_manager` and `ntp_manager`. That is a dependency pointing *upward*,
from the link layer to the services running over it: WiFi could not be built without both,
and neither could be used without WiFi. Now `PRIV_REQUIRES` names only the driver, netif,
event loop, timer, NVS and `net_events`.

It also means the link need not be WiFi. An Ethernet driver or a cellular modem posting
the same events serves mDNS, NTP and MQTT unchanged.

## Configuration

The schema is `wifi_manager_config_schema.json`; the generated `wifi_manager_config_t` is
this component's public config type.

Note the name: a schema `$id` of `wifi_config` would generate `wifi_config_t`, which is
already **ESP-IDF's own union** for `esp_wifi_set_config()`.

Notable fields:

- `known_networks` — up to four SSIDs with a `priority`; the highest-priority visible
  network wins a scan.
- `ap_ssid` / `ap_password` — the access point served when no known network is reachable,
  so a device that cannot join anything is still configurable.
- `reachability_host` — optional. A station can be associated and hold a DHCP lease while
  reaching nothing at all; pinging a known host is how that is detected. **Empty disables
  it**, which is right for an isolated network — the predecessor hardcoded a host on the
  author's own LAN.
- `connection_timeout_ms` — re-armed on association as well as on connect, so a stalled
  DHCP cannot wedge the state machine in CONNECTING.

## Behaviour worth knowing before editing

- **Scan results must be consumed or cleared.** They pin driver memory until fetched;
  leaving them makes every later `esp_wifi_scan_start()` fail with `ESP_ERR_WIFI_STATE`.
  The `SCAN_DONE` handler clears them when it will not use them.
- **The reachability ping runs on its own task** because it starts with a DNS lookup that
  blocks. It re-checks the connection state after the lookup and aborts if the link went
  away while it was in flight.
- **A leftover ping session is deleted before a new one is created**, rather than
  overwriting the handle and leaking it.
