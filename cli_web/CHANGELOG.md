# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- HTTP + WebSocket transport for `cli`, with the console page embedded via `EMBED_FILES`.

Extracted from `esp_board_bringup`'s `main/web/`.

### Changed from the original

- **The HTTP server is injectable.** `cli_web_config_t.server` registers the handlers on a
  server the caller already runs; passing NULL starts and owns one. A device with its own
  web interface must not end up with two listeners, and a borrowed server keeps
  authentication and error handling in one place. `cli_web_stop()` stops the server only
  if it started it.
- **mDNS is gone.** The original called `mdns_init()` and advertised `esp-bringup` with a
  fixed instance name. Which name a device answers to is project policy, not a property of
  the console, and it belongs to whatever component owns mDNS.
- **URIs are configurable** (`page_uri`, `ws_uri`), and the page can be turned off
  entirely with `serve_page = false` for a project supplying its own UI.
- **The page is project-neutral**: it titles itself from `location.host`, so it names the
  device you are actually attached to, and it selects `wss://` when served over HTTPS.
- Frame size, socket count and the maximum accepted command length became Kconfig options.
- `cli_web_start()` returns `esp_err_t` and unwinds cleanly on failure instead of logging
  and leaving a half-registered server behind.
