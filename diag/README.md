# diag

Command and log output fan-out. One formatted line reaches stdout **and** every registered
sink, so a serial port, a browser and an MQTT topic all show the same text — including
output from a command that was typed somewhere else.

```c
diag_init(NULL);                 /* also routes ESP_LOGx through the fan-out */
diag_printf("bus %d: %d devices\n", bus, n);
diag_error("No device at 0x%02x", addr);   /* prints "ERR: ..." and a newline */
```

Kept separate from [`cli`](../cli/) on purpose: the fan-out, not the shell, is what makes
logging to a filesystem, to MQTT or to a web page possible, and a headless project wants
that without a REPL. `diag` depends on nothing.

## Sinks

```c
static void ws_sink(const char *text, size_t len, void *ctx);
diag_sink_register(ws_sink, server);
```

A sink receives already-formatted text and **must not call `diag_printf()`** — the fan-out
holds its lock across the call, so re-entering it recurses until the stack is gone.

Sinks live with their transport, not here: an MQTT log sink belongs to the MQTT component,
a file sink to whatever owns the filesystem. That is what keeps this component's dependency
list empty.

## Why `ERR:`

`diag_error()` prefixes every failure line with `ERR: `, so a host-side script driving the
device can tell success from failure without parsing prose. Command code should use it for
every failure and return `-1`.

## Configuration

`CONFIG_DIAG_MAX_SINKS` (default 4) and `CONFIG_DIAG_FORMAT_BUF_SIZE` (default 512). Both
are statically allocated. A line longer than the format buffer is truncated, not split.
