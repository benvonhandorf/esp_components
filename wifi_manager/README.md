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
- `reconnect_delay_ms` / `scan_interval_ms` — after the link drops, the first reconnect
  scan waits `reconnect_delay_ms` (1 s); each further scan doubles that, up to
  `scan_interval_ms` (2 min).
- `power_save` — off by default. Modem sleep costs missed beacons and latency on a weak
  link; turn it on for a battery device.
- `roaming` — on by default; advertises 802.11k/v so the access point can steer the
  station. **Needs `CONFIG_ESP_WIFI_11KV_SUPPORT=y` in the application's
  `sdkconfig.defaults`**, or it does nothing.

## Choosing an access point

A connect uses an all-channel scan sorted by signal, so on an SSID served by several
access points the station joins the strongest. The default fast scan joins the *first*
match — on a mesh, whichever access point sits on the lowest channel, however far away.
No BSSID is pinned: a pinned BSSID cannot fail over, and the driver clears it anyway once
BSS transition management is enabled.

A BSS transition (disconnect reason 12, or 207) while connected is a roam, not a lost
link: the supplicant is already connecting to the new access point, so no scan is started
and `NET_EVENT_LINK_DOWN` is not posted. `NET_EVENT_LINK_UP` is posted again when the
address is confirmed, which subscribers already have to tolerate.

## Reporting why the link dropped

```c
wifi_manager_link_info_t link;
wifi_manager_get_link_info(&link);
/* link.connected, link.bssid, link.channel, link.rssi,
 * link.link_losses, link.roams,
 * link.last_disconnect_reason (wifi_err_reason_t), link.last_disconnect_rssi,
 * link.last_disconnect_us */
```

Useful reasons: 200 beacon timeout, 201 no AP found, 15 4-way handshake timeout, 8 left
(this station disconnected), 4 inactivity.

## Showing what is nearby

```c
wifi_ap_record_t aps[32];
uint16_t found = 0;
if (wifi_manager_scan(aps, 32, &found) == ESP_OK) {
    for (uint16_t i = 0; i < found; i++) {
        printf("%-32s %4d dBm  ch %d\n", (const char *)aps[i].ssid,
               aps[i].rssi, aps[i].primary);
    }
}
```

**Do not call `esp_wifi_scan_start()` yourself.** It is not a matter of taste: this
component's `WIFI_EVENT_SCAN_DONE` handler either consumes the records to look for a
known network or clears them to release driver memory, and it runs before a blocking
`esp_wifi_scan_start()` returns to its caller. Your scan comes back with zero access
points while this component's log lists the ones it just found — and nothing about that
looks like a bug from the caller's side. Scanning has to belong to whoever owns the
handler.

The scan blocks for a second or two, and the manager gives up that scan's own chance to
join a known network; the reconnect timer is re-armed so the attempt is not lost.

## Behaviour worth knowing before editing

- **Scan results must be consumed or cleared.** They pin driver memory until fetched;
  leaving them makes every later `esp_wifi_scan_start()` fail with `ESP_ERR_WIFI_STATE`.
  The `SCAN_DONE` handler clears them when it will not use them, and steps aside entirely
  while `wifi_manager_scan()` is waiting for them.
- **The reachability ping runs on its own task** because it starts with a DNS lookup that
  blocks. It re-checks the connection state after the lookup and aborts if the link went
  away while it was in flight.
- **A leftover ping session is deleted before a new one is created**, rather than
  overwriting the handle and leaking it.

## Known bugs

- **`priority` is documented but never read.** `connect_from_scan_results()` picks the
  known network whose strongest visible access point has the best RSSI; the `priority`
  field of `known_networks` takes no part in it, so a lower-priority network that happens
  to be nearer wins. The field is `required` in the schema, which makes it look load
  bearing. Either the selection should order by `priority` first and use RSSI to break
  ties, or the field should go — but it cannot quietly stay as decoration, because "the
  highest-priority visible network wins a scan" is what the Configuration section above
  promises. No effect on a device with a single known network, which is why it went
  unnoticed.

  `wifi_manager_add_known_network()` sets `priority = 0` on every network it adds, which
  is the same gap seen from the other side: there is no argument for it because nothing
  consumes it.
