# ina237

Driver for the TI INA237, an I2C current, voltage and power monitor that
measures across an external shunt. Shunt calibration, the full measurement set,
and the health bits that say when a reading cannot be trusted.

The A0/A1 pins select one of sixteen addresses, `0x40`–`0x4F` (Table 7-2), so a
bus can carry several.

## Installing

```yaml
# your project's idf_component.yml
dependencies:
  ina237:
    git: https://github.com/benvonhandorf/esp_components.git
    path: ina237
    version: ina237-v0.1.0
```

Requires ESP-IDF 5.3 or later, for the `i2c_master` driver.

## Using it

The driver does not own the I2C bus and does not create a device on it. You pass
in a device handle you made, and you keep owning it — so the same bus can carry
other parts, managed however your application already manages them.

```c
#include "ina237.h"

i2c_master_dev_handle_t dev;
const i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x40,
    .scl_speed_hz    = 100000,
};
ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

ina237_handle_t ina;
const ina237_config_t cfg = { .dev = dev, .shunt_ohms = 0.004 };
ESP_ERROR_CHECK(ina237_create(&cfg, &ina));

ina237_config_report_t report;
esp_err_t err = ina237_configure(ina, 0.004, &report);
if (err == ESP_ERR_INA237_WRONG_PART) {
    /* Something answered, but MANUFACTURER_ID read report.manufacturer_id. */
}

ina237_reading_t r;
if (ina237_read(ina, &r) == ESP_OK) {
    printf("%.3f V  %.4f A  %.3f W\n", r.bus_v, r.current_a, r.power_w);
}
```

If your I2C layer caches device handles, call `ina237_set_device()` before each
use. A cache that recycles entries, or a bus that is torn down and rebuilt,
leaves a previously held handle dangling.

## It returns facts, not text

Nothing here formats a message. `ina237_configure()` collapses a probe and a
register write behind one call, so its report carries an `ina237_stage_t` naming
which of them failed — without that, a NACK on the `SHUNT_CAL` write and a part
that is not an INA237 at all come out as the same "configuration failed", and
only one of those is something the user wired wrong.

`ina237_read()` decodes the health bits into three separate facts rather than
handing back one register, because they mean different things:

- `trim_checksum_ok` false (DIAG_ALRT.MEMSTAT) — the trim memory is corrupt and
  **no** reading can be trusted.
- `math_overflow` true (DIAG_ALRT.MATHOF) — current and power specifically are
  invalid. Bus voltage and die temperature are still good.
- `shunt_cal_matches` false — `SHUNT_CAL` no longer holds what `configure()`
  wrote, so the part has been reset since. Current and power are scaled by the
  wrong constant and look entirely plausible.

## Things this part does that will not look like errors

- **`SHUNT_CAL` is the same constant for every shunt.** Equation 1 says
  `SHUNT_CAL = 819.2e6 × CURRENT_LSB × RSHUNT`. Choosing the maximum current so
  it exactly fills the ADC range makes `CURRENT_LSB = SHUNT_FULL_SCALE_V /
  RSHUNT / 2^15`, and substituting that back cancels `RSHUNT` entirely — leaving
  4096, which is also the register's reset value. So a part that was reset still
  reads back the *right* `SHUNT_CAL`, and the mismatch check only catches a
  reset to some *other* value. The shunt lives in this driver's scaling, not in
  the device.
- **There is no DEVICE_ID register.** The map ends at `MANUFACTURER_ID`, unlike
  the otherwise similar INA238 and INA228. `MANUFACTURER_ID` reads the ASCII
  `"TI"` (`0x5449`) and is the only identification available — it confirms the
  vendor, not the part number, so an INA238 fitted by mistake passes this check.
- **The register pointer does not auto-increment** (section 7.5.1.1). Every
  value is its own write-then-read transaction; a burst read across registers
  returns the same register repeatedly.
- **`DIETEMP` is a signed 12-bit value in bits 15:4.** It has to be
  sign-extended as 16 bits *first* and then shifted down. Shifting first
  discards the sign and turns every sub-zero temperature into a large positive
  one.
- **This driver never writes `CONFIG`,** so `ADCRANGE` stays at its reset value
  of 0 and the full-scale constants in Table 8-1 apply as written. A part left
  at `ADCRANGE = 1` by other software reads shunt voltage four times too small.

## License

Not yet declared. The repository this component lives in has no LICENSE file, so
`idf_component.yml` carries no `license:` key either; that is a gap to close
before the first tag, not a claim that the code is unencumbered.
