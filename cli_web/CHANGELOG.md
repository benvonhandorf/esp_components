# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.2.2] - 2026-09-17

### Changed

- Names `cli-v0.2.1` and `diag-v0.1.1`. No code change: both are pin moves, so
  that a project consuming cli_web, cli, mqtt_manager and diag resolves one ref
  per component rather than two.

## [0.2.1] - 2026-09-06

### Changed

- Names `cli-v0.2.0`, for the help-text resolver. Tagged without this entry at
  the time; recorded here because a released version with no changelog line is
  indistinguishable from one nobody wrote down.

## [0.2.0] - 2026-09-05

### Fixed

- **The output sink deadlocked the TCP/IP thread.** `diag` calls its sinks on whatever
  task produced the line, holding its output lock, and that task can be lwIP's TCP/IP
  thread: `ESP_LOGx` from an lwIP raw-API callback -- `ntp_manager`'s SNTP sync
  notification, for one -- arrives there. The sink called `httpd_queue_work()`, which is a
  `sendto()` on the server's control socket, and with `CONFIG_LWIP_TCPIP_CORE_LOCKING`
  disabled every socket call posts to the TCP/IP mailbox and waits for the TCP/IP thread
  to run it. That thread was already inside the sink, so it waited for itself: a permanent
  deadlock holding `diag`'s output lock, which then swallowed the next `diag_printf()` on
  any task. The visible symptom was the shell hanging on its first line of output, on a
  device whose network had silently stopped.

  Output is now copied onto a bounded queue and delivered by a task of its own, so the
  sink blocks on nothing and enters lwIP from nowhere.

- **A failed send fed itself.** `httpd_ws_send_frame_async()` warns when a socket has gone
  away, and that warning came back through the sink to be sent to the same socket, warn
  again, and repeat -- a loop that sustained itself for as long as one browser's socket
  stayed broken. The two tasks in the delivery path, the broadcaster and the server task,
  now drop what they log themselves rather than queueing it. `diag` still writes those
  lines to the serial console directly.

- **A borrowed server larger than `CLI_WEB_MAX_OPEN_SOCKETS` silenced every browser.** The
  client-list buffer was sized from this component's own Kconfig, but a borrowed server
  keeps whatever its owner configured -- and `httpd_get_client_list()` does not truncate to
  the space offered, it fails the whole call. One connection past that number and
  `broadcast_work()` sent to nobody. It is now sized from `CONFIG_LWIP_MAX_SOCKETS`, the
  ceiling `httpd` itself enforces on any server's `max_open_sockets`.

### Added

- `cli_web_dropped()`, counting chunks lost rather than shown. Non-zero means the
  browser's view of the log has holes in it; the serial console is always complete.
- `CLI_WEB_BROADCAST_QUEUE_LEN` and `CLI_WEB_TX_STACK` for the new broadcaster task.

### Changed

- **Start it at boot; it no longer needs an address.** Output produced before a browser
  connects is dropped, which it was anyway.

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
