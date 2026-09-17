# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.1] - 2026-09-17

### Changed

- Names `http_server-v0.2.0`, which listens from `start()` instead of waiting for
  a link. `ota_http` only calls `http_server_add_routes()`, which is unchanged, so
  nothing here changes; the pin exists so a project cannot be handed two
  `http_server` refs to resolve in one build.

## [0.1.0] - 2026-09-04

### Added

- Transport-independent update session (`ota_session_begin/write/end/abort`).
- Image header and app-descriptor validation after roughly the first kilobyte.
- A staged report saying which step failed, so a caller can say what to do.
- Rollback support: `ota_mark_valid()`, `ota_pending_verify()`,
  `ota_rollback_and_reboot()`.
- An optional authenticated HTTP POST route.

Extracted from `solar_power_monitor`'s `components/http_server/ota_handler.c`.

### Changed from the original

- **The transport is separable.** The original was a single function taking an
  `httpd_req_t *`, so an update over MQTT or a serial console meant rewriting it.
- **The image is validated before the whole upload is taken.** The original wrote
  whatever arrived and only found out at `esp_ota_end()`. Uploading the wrong binary is
  the common accident, and it is now rejected after about a kilobyte, naming the project
  the image was actually built for.
- **Failures say which step failed.** Every path returned a generic 500; a rejected
  signature and a partition too small need completely different responses.
- **Rollback is supported.** The original set the boot partition and restarted, with no
  way to confirm the new image or return to the old one.
- Signature verification is delegated to ESP-IDF: with
  `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, an image not signed by the project's
  key fails at `OTA_STAGE_END`. The original README noted that no signing was used.
