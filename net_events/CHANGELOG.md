# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- `NET_EVENT` base with link-up/down, AP started/stopped and time-synced events.

New in extraction. In `solar_power_monitor` the WiFi manager called mDNS and SNTP
directly from its own event handler, so `networking` had to `REQUIRES mdns_manager`
and `ntp_manager` — a dependency pointing upward from the link layer to the services
running over it, which made WiFi unbuildable without both and either unusable without
WiFi. Inverting it onto an event base also removes the application's hand-wiring of
`IP_EVENT_STA_GOT_IP` to `mqtt_manager_start`.
