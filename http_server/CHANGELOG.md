# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- Route registration as data, with per-route Basic authentication.
- Listens on network link-up, including over the device's own access point.
- `http_server_handle()`, so `cli_web` and others share one server.
- Host tests for Base64 encoding and credential comparison.

Extracted from `solar_power_monitor`'s `components/http_server`.

### Changed from the original

- **Routes are registered, not overridden.** A weak
  `http_server_register_services(httpd_handle_t *, http_config_schema_t *, QueueHandle_t)`
  depended on link order and could not serve a component's own route. Its signature also
  passed the application's relay queue through this component, which is why the component
  could not be reused.
- **The credential check does not decode the header.** The original hand-rolled a Base64
  decoder over attacker-controlled input — treating invalid characters as zero, and
  finding `'\0'` in its alphabet lookup would have yielded index 64 — and compared the
  result with `strcmp`. The expected header is now encoded once from our own credentials
  and compared in constant time.
- **Authentication defaults to on, and there is no default password.** It defaulted to
  off with `admin`/`admin`; starting with authentication enabled and no password is now
  refused rather than defaulted or silently disabled.
- **The realm is configurable** rather than the literal `"Solar Monitor"`.
- **The application's types are gone**: the header included `system_status.h`, which it
  never used, and the CMakeLists required `relay_control`, which it never called.
- OTA is no longer built in. It was a hardcoded `/ota` route here; it now lives in the
  `ota` component and registers itself as a route like anything else.
