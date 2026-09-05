# lm75bdp

NXP LM75B temperature sensor and thermal watchdog over I2C.

```c
lm75bdp_config_t cfg = { .dev = i2c_device_handle(LM75BDP_I2C_ADDR_DEFAULT) };
lm75bdp_handle_t lm;
lm75bdp_create(&cfg, &lm, NULL);

lm75bdp_reading_t r;
lm75bdp_read(lm, &r);            /* r.temperature_C, 0.125 C resolution */

/* The OS output asserts above tos and releases below thyst. The gap is the
 * hysteresis; equal values make the output chatter around the threshold. */
lm75bdp_report_t report;
lm75bdp_set_thresholds(lm, 60.0f, 55.0f, &report);
printf("programmed %.1f / %.1f C\n", report.tos_C, report.thyst_C);
```

Thresholds are quantised to 0.5 C and clamped to -128..+127.5 C, so the report says what
was actually programmed rather than what was asked for.

The encoding is host-tested. The original truncated toward zero when converting a
threshold, so -0.7 C became -0.5 rather than -1.0 — a threshold on the wrong side of the
temperature it was meant to catch, and only below freezing, where a bench test at room
temperature would never find it.

## It does not own the bus

`cfg.dev` is a device handle the caller created and owns, and it **may be NULL** — the
handle is still created, and every call needing the bus returns `ESP_ERR_INVALID_STATE`
until `lm75bdp_set_device()` supplies one. That lets a handle exist before anything is
powered.

A bus that is torn down and rebuilt invalidates every device handle taken from it, so
whoever owns the bus calls `lm75bdp_set_device()` again rather than this driver caching a
handle that can dangle.

## Tests

```sh
make -C test
```
