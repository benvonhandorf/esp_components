# rx8130ce

Epson RX8130CE real-time clock with battery backup.

```c
rx8130ce_config_t cfg = { .dev = i2c_device_handle(RX8130CE_I2C_ADDR_DEFAULT) };
rx8130ce_handle_t rtc;
rx8130ce_report_t report;
rx8130ce_create(&cfg, &rtc, &report);

struct timeval tv;
esp_err_t err = rx8130ce_get_time(rtc, &tv);
if (err == ESP_ERR_RX8130CE_NOT_SET) {
    /* The part is there and answering; it just does not know the time. */
}
```

## A clock that does not know the time says so

A part whose backup has drained still answers, and still returns registers -- they simply
do not decode to a date. Returning that as a time would let a dead RTC pass for a device
that believes it is the year 2000. `rx8130ce_get_time()` returns
`ESP_ERR_RX8130CE_NOT_SET` instead, and the part's own voltage-low flags are surfaced as
`report.power_lost`.

Setting the time clears those flags, so a clock that has been set stops reporting itself
unreliable.

## UTC

The part holds UTC and the driver converts with `timegm()`, not `mktime()` -- the latter
applies whatever timezone happens to be set and shifts the result by it.

## It does not own the bus

`cfg.dev` is a device handle the caller created and owns, and it **may be NULL** -- the
handle is still created, and every call needing the bus returns `ESP_ERR_INVALID_STATE`
until `rx8130ce_set_device()` supplies one. That lets a handle exist before anything is
powered.

A bus that is torn down and rebuilt invalidates every device handle taken from it, so
whoever owns the bus calls `rx8130ce_set_device()` again rather than this driver caching a
handle that can dangle.

## Tests

```sh
make -C test
```
