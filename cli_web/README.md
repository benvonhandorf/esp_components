# cli_web

Browser transport for the [`cli`](../cli/) shell. Serves a single-page terminal and carries
command lines over a WebSocket.

```c
/* Once the device has an address. */
cli_web_start(NULL);            /* own server on :80, page at "/", socket at "/ws" */
```

Commands typed in the browser join the **same executor queue** the serial reader feeds, and
a `diag` sink mirrors all output back to every connected browser. So a command typed on the
serial port shows its output in the browser, and vice versa — one session, two windows.

## Sharing an HTTP server

A device that already serves a web interface must not end up with two listeners. Pass yours:

```c
cli_web_config_t cfg = {
    .server    = my_server,     /* NULL starts and owns one */
    .page_uri  = "/console",
    .ws_uri    = "/ws",
    .serve_page = true,
};
cli_web_start(&cfg);
```

`cli_web_stop()` unregisters the handlers and stops the server **only if it started it**.

Two requirements on a borrowed server: `CONFIG_HTTPD_WS_SUPPORT=y`, and enough
`max_open_sockets` — every open browser tab holds a WebSocket for as long as it is on the
page, so the httpd default of 4 runs out with a couple of tabs.

The built-in page connects to `/ws` specifically. If you change `ws_uri`, set
`serve_page = false` and serve your own.

## Requires `CONFIG_HTTPD_WS_SUPPORT=y`

`esp_http_server` compiles the WebSocket API out by default. Without it the build fails on
the `httpd_ws_*` functions being absent.

## Notes on the design

- **No mDNS.** Which name a device answers to is project policy, not a property of a
  console. Advertise the service from whatever component owns mDNS.
- **The page is embedded** via `EMBED_FILES`, so the console works with no filesystem and
  no partition to keep in step with the firmware. It titles itself from `location.host`,
  so it names the device you are attached to, and selects `wss://` when served over HTTPS.
- **The WebSocket handler does not echo.** Calling `diag_printf()` there would run the
  output sink — and therefore `httpd_queue_work()` — on the HTTP server's own task, which
  risks deadlocking against a full control queue. The browser echoes its own input locally.
- **Output is dropped rather than blocking.** If an allocation for a frame fails, the chunk
  is discarded: stalling the shell to guarantee a browser sees every byte is the wrong
  trade for a diagnostic interface.
