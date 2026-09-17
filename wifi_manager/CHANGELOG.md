# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.3.0] - 2026-09-17

Found soak-testing a stationary ESP32-C3 on a two-access-point network: 86 reconnect
events in six hours, with RSSI swinging between -56 and -82 dBm on a board that never
moved.

### Fixed

- **The station joined the first access point it heard, not the strongest.** The
  connect config left `scan_method` zeroed, which is `WIFI_FAST_SCAN`: the driver stops
  at the first BSS matching the SSID, and `sort_method` is only honoured by an
  all-channel scan. The manager's own scan compared RSSI but kept only which *network*
  won, so on an SSID served by several access points the device joined whichever sat
  on the lowest channel. Connects now use `WIFI_ALL_CHANNEL_SCAN` with
  `WIFI_CONNECT_AP_BY_SIGNAL`. No BSSID is pinned, so a failed access point is still
  failed over from.
- **Every lost link waited `scan_interval_ms` (default two minutes) before its first
  scan.** Reconnect scans now back off from the new `reconnect_delay_ms` (default 1 s),
  doubling per scan up to `scan_interval_ms`, and reset once an address is obtained.
- **Modem power saving was on by default.** The driver's `WIFI_PS_MIN_MODEM` sleeps the
  radio between DTIM beacons, which on a marginal link means missed beacons, beacon
  timeout disconnects and socket stalls for whatever runs over the link. Station mode
  now sets `WIFI_PS_NONE` unless `power_save` is true.

### Added

- `roaming` (default true): sets `rm_enabled` and `btm_enabled`, so an 802.11k/v
  access point can steer the station. Needs `CONFIG_ESP_WIFI_11KV_SUPPORT=y` in the
  application; without it the flags are inert.
- A BSS transition (disconnect reason 12, or 207 from Espressif's roaming app) while
  connected is no longer handled as a lost link: no reconnect scan is started to
  collide with the supplicant's own connect, and `NET_EVENT_LINK_DOWN` is not posted.
  The connection timeout still applies, so a roam that goes nowhere becomes an ordinary
  disconnect.
- `wifi_manager_get_link_info()`: the associated BSSID, channel and RSSI, counts of
  lost links and roams since boot, and the reason, RSSI and time of the last
  disconnect -- so a device whose serial port resets it can report why it dropped.
- Config properties `reconnect_delay_ms`, `power_save` and `roaming`, all defaulted.
  `scan_interval_ms` now means the backoff's ceiling.

## [0.2.1] - 2026-09-06

### Fixed

- An empty `wifi` section parsed to a zeroed struct instead of the schema
  defaults, and a zeroed `scan_interval_ms` scans without pause. `hostname` was
  required with no default, and the generated parser reports a missing required
  field and returns *before* it applies any of the other defaults -- so
  `{"wifi":{}}`, the document an application falls back to when there is no
  configuration file, failed to parse and left every field zero. The caller then
  runs on that: `schedule_reconnect()` arms a one-shot timer for 0 ms, the timer
  fires as soon as the scan that armed it finishes, and the radio scans back to
  back for as long as the device is up. `hostname` now defaults to `esp-device`
  and is no longer required, so an empty section yields a working configuration,
  which is what the required/default distinction is actually for.

- `scan_interval_ms` and `connection_timeout_ms` are clamped to their schema
  minimums in `wifi_manager_init()`, with one warning apiece. The struct comes
  from the caller, not from the parser, so the schema's `minimum` is a promise
  about parsed configurations and not about what arrives at the door.
  `schedule_reconnect()` repeats the floor silently: it is the one call that can
  turn a bad value into an unbounded scan loop.

## [0.2.0] - 2026-09-05

### Added

- `wifi_manager_scan()`, for a user interface that wants to show what is nearby
  rather than join it. A caller cannot do this alone: a scan started outside the
  component races the manager's own `WIFI_EVENT_SCAN_DONE` handler, which
  consumes the records to look for a known network or clears them to release
  driver memory. The handler wins every time, so the caller's blocking scan
  returns an empty list while the manager logs the access points it just found.
  Whoever owns the event handler has to own scanning.

  `wifi_ap_record_t` appears in the public header, so `esp_wifi` moves from
  `PRIV_REQUIRES` to `REQUIRES`.

### Fixed

- `schedule_reconnect()` logged `scan_interval_ms`, a `uint32_t`, with `PRIu64`.
  The variadic read took two slots and printed whatever followed it -- values
  like "reconnect scan in 3689918353324026336 ms" on every disconnect. The timer
  was always armed correctly; only the line saying so was wrong, which is worse
  than it sounds on a bench, where that line is the evidence that reconnection is
  working.

## [0.1.0] - 2026-09-04

### Added

- Station and AP modes, prioritised known networks, scan-and-connect, connection
  timeouts, reconnect backoff and an optional reachability ping.

Extracted from `solar_power_monitor`'s `components/networking`.

### Changed from the original

- **The dependency graph was inverted.** It called `mdns_manager_init()` and
  `esp_netif_sntp_init()` from `wifi_manager_init()`, and `mdns_on_connect()` /
  `mdns_on_disconnect()` from its event handler, so `networking` had to
  `REQUIRES mdns_manager ntp_manager` — upward, from the link layer to the services
  above it. It now posts `net_events` and knows about neither. (The mDNS calls it was
  making were both no-ops, so nothing was lost.)
- **`wifi_manager_init()` no longer takes an NTP config**, and the SNTP client and
  time-sync callback moved to `ntp_manager`, where the clock belongs. They lived here
  only because this file happened to be where `esp_netif_sntp` was initialised — with a
  `TODO: Move NTP server to config` above a hardcoded server.
- **The reachability host is configuration, not a constant.** It was
  `getaddrinfo("littlerascal.local", ...)`, a host on the author's own network, with no
  way to disable the check. Empty now disables it.
- **The generated config type is `wifi_manager_config_t`**, not `wifi_config_t`, which
  is ESP-IDF's own union for `esp_wifi_set_config()`. The original avoided the collision
  by accident, through a `_schema` suffix on its `$id`.
- **AP credentials no longer default to the reference project's device name**
  (`powermonitor1`).
- Integer fields carry a `js2cType`, so a WiFi channel constrained to 1..13 is a
  `uint8_t` rather than the generator's default `uint64_t`.
