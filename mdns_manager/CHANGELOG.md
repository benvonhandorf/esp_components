# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- Hostname and instance advertising, service registration, and re-advertising after a
  reconnect.

Extracted from `solar_power_monitor`'s `components/mdns_manager`.

### Changed from the original

- **Services are registered, not overridden.** A weak `mdns_manager_register_services()`
  that the application replaced with a strong symbol depended on link order — the
  application needed a `volatile` function pointer purely to keep the override — and gave
  a component no way to advertise its own service. `mdns_manager_add_service()` takes a
  descriptor, which is copied so it may be a temporary.
- **It listens instead of being called.** `mdns_manager_init()` was invoked from inside
  `wifi_manager_init()`, and `mdns_on_connect()`/`mdns_on_disconnect()` from the WiFi
  event handler — both of which were empty functions, so nothing actually happened on a
  reconnect. It now subscribes to `net_events` and re-advertises for real.
- **It advertises over the device's own access point**, which the original did not.
- TXT records are supported.
