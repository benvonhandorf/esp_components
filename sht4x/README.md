# sht4x

Driver for the Sensirion SHT4x humidity and temperature sensor. Measurement at
all three repeatabilities, the six heater settings, serial number and soft
reset, with CRC checking that says which word failed.

The variants differ only in their fixed address — A=`0x44`, B=`0x45`, C=`0x46`.
There are no address pins, so the address tells you which part was fitted rather
than how it was strapped.

## Installing

```yaml
# your project's idf_component.yml
dependencies:
  sht4x:
    git: https://github.com/benvonhandorf/esp_components.git
    path: sht4x
    version: sht4x-v0.1.0
```

Requires ESP-IDF 5.3 or later, for the `i2c_master` driver.

## Using it

The driver does not own the I2C bus and does not create a device on it. You pass
in a device handle you made, and you keep owning it — so the same bus can carry
other parts, managed however your application already manages them.

```c
#include "sht4x.h"

i2c_master_dev_handle_t dev;
const i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = SHT4X_ADDR_DEFAULT,
    .scl_speed_hz    = 100000,
};
ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

sht4x_handle_t sht;
const sht4x_config_t cfg = { .dev = dev };
ESP_ERROR_CHECK(sht4x_create(&cfg, &sht));

sht4x_measurement_t m;
esp_err_t err = sht4x_measure(sht, SHT4X_REPEATABILITY_HIGH, &m);
if (err == ESP_OK) {
    printf("%.2f C  %.2f %%RH\n", m.temperature_c, m.humidity_pct_cropped);
} else if (err == ESP_ERR_SHT4X_CRC) {
    printf("word %d: got 0x%02X, computed 0x%02X\n",
           m.crc_error.word, m.crc_error.received, m.crc_error.computed);
}
```

This part carries no per-device state, so **one handle can serve every address
on the bus** by being re-pointed with `sht4x_set_device()` between calls. If your
I2C layer caches device handles you must do that in any case: a cache that
recycles entries, or a bus that is torn down and rebuilt, leaves a previously
held handle dangling.

## It returns facts, not text

`sht4x_measure()` reports humidity twice — `humidity_pct` as the equation
produced it and `humidity_pct_cropped` clamped to 0–100 — plus
`humidity_was_cropped`. The datasheet expects the value to be cropped, and for
a product that is right, but during bringup a reading of 112 %RH means something
is actually wrong and cropping it to 100 hides the one signal worth having. The
driver refuses to choose; the caller does.

CRC failures likewise return `ESP_ERR_SHT4X_CRC` with a `sht4x_crc_error_t`
naming the word, the byte received and the byte computed. A bare "CRC failed"
cannot distinguish one corrupted byte on a marginal bus from a part that does
not implement this CRC at all, and those want different responses.

## Things this part does that will not look like errors

- **A repeated-start write-then-read does not work.** There is no register
  pointer. The command byte is one transaction, then the sensor needs time to
  measure, then the result is a *separate* read transaction. Issuing the read
  too early — which is exactly what `i2c_master_transmit_receive()` does — makes
  the sensor NACK the read header (Table 8). That reads as an absent part.
- **`vTaskDelay(n)` is not a delay of n ticks.** It guarantees only `n-1` full
  tick periods, because the first tick boundary can arrive immediately. With a
  10 ms tick, `vTaskDelay(pdMS_TO_TICKS(10))` can return almost at once and let
  the read overtake an 8.3 ms measurement — producing the NACK above,
  intermittently. This driver adds one tick to every wait for that reason.
- **There is no ID register.** A serial number that reads back with valid CRCs
  is the only available evidence that a real sensor is present, which is why
  `sht4x_read_serial()` reports CRC detail rather than swallowing it.
- **The heater commands are measurements.** Each runs the heater and then takes
  a high-precision reading just before switching it off, so `sht4x_run_heater()`
  returns a `sht4x_measurement_t` like `sht4x_measure()` does. It also blocks
  for the whole pulse — up to about 1.2 s for the 1 s settings.
- **Only six power/duration pairs exist** (20/110/200 mW × 100/1000 ms). Any
  other combination is not a rounding matter; the device implements no command
  for it. `sht4x_heater_from_values()` is the whole legal set.

## License

Not yet declared. The repository this component lives in has no LICENSE file, so
`idf_component.yml` carries no `license:` key either; that is a gap to close
before the first tag, not a claim that the code is unencumbered.
