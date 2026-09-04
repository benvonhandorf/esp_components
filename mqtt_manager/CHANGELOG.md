# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- Topic-handler registry with proper MQTT wildcard matching, re-applied on reconnect.
- Last-will and retained birth messages.
- `$DEVICE$` substitution in the topic prefix.
- `mqtt_log_sink`, forwarding console and log output to a topic.
- Host tests for topic matching and topic building.

Extracted from `solar_power_monitor`'s `components/mqtt_manager`.

### Changed from the original

- **The application's domain is gone.** It included `relay_control.h` and
  `system_status.h`, hardcoded `MQTT_TOPIC_SUFFIX_RELAYS`, parsed relay JSON in its event
  handler and took a `QueueHandle_t relay_queue` in its init. Its header guard was
  `SOLAR_MQTT_CLIENT_H`. Domain knowledge now enters through `subscribe()`/`publish()`,
  and `mqtt_manager_publish_status(system_status_t *)` does not exist here — the status
  message belongs to the project.
- **Subscriptions survive a reconnect.** The original re-subscribed its single hardcoded
  topic; a registry has to re-subscribe all of them, and does.
- **Wildcards are matched, not compared.** Dispatch was
  `strncmp(event->topic, MQTT_TOPIC_RELAYS, ...)`, so a `+` or `#` subscription would have
  received nothing.
- **It connects itself.** The application registered an `IP_EVENT_STA_GOT_IP` handler to
  call `mqtt_manager_start()`, and a `WIFI_EVENT_STA_DISCONNECTED` handler to call
  `mqtt_manager_note_link_down()`. Both are now `net_events` subscriptions here.
- **Last will added.** The project README called for a topic that makes it easy to tell
  whether the device is online; there was no LWT in the code.
- **`$DEVICE$` implemented.** The original schema documented the substitution and the code
  did a plain `snprintf("%s%s", prefix, suffix)`.
- **Keepalive is configurable** rather than a hardcoded 120, and an empty username now
  means an anonymous connection rather than offering an empty username.
