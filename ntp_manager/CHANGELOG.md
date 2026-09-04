# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- SNTP client started on network link-up, posting `NET_EVENT_TIME_SYNCED`.
- POSIX timezone applied before the first sync.
- `ntp_manager_is_synced()` / `ntp_manager_last_sync()`.

### Notes

The component of this name in `solar_power_monitor` was a stub: `ntp_manager_init()` had
an empty body with no return statement, and was never called. The actual SNTP setup lived
in `wifi_manager.c` behind a `TODO: Move NTP server to config`, against a hardcoded
server. This is that logic implemented where it belongs, with the server configurable.

Guards against `CONFIG_LWIP_SNTP_MAX_SERVERS` (default 1) rather than writing a second
server past the end of `esp_sntp_config_t`.
