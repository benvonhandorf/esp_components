# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
