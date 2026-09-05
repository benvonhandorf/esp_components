# ina219

TI INA219 bidirectional current, voltage and power monitor over I2C.

## Installing

```yaml
dependencies:
  ina219:
    git: https://github.com/benvonhandorf/esp_components.git
    path: ina219
    version: ina219-v0.1.0
```

## Using it

```c
ina219_config_t cfg = {
    .dev           = i2c_device_handle(INA219_I2C_ADDR_DEFAULT),
    .shunt_ohms    = 0.1f,     /* the resistor actually fitted */
    .max_current_a = 3.2f,     /* the largest current to measure */
};

ina219_handle_t ina;
ina219_report_t report;
ina219_create(&cfg, &ina, &report);
printf("resolution: %.3f mA/bit\n", report.current_lsb_a * 1000.0f);

ina219_reading_t r;
ina219_read(ina, &r);
```

## Read this if you are replacing the old driver

**Current and power readings will change by about 24.5%.**

The driver this replaces wrote a calibration register of 51 for a 0.1 Ω shunt and then
scaled every current reading by 10 mA/bit. Those two do not agree: CAL = 51 on 0.1 Ω
yields **8.03 mA/bit**, so readings were 10 / 8.031 = 1.245× the true value.

Its comment read *"R100 shunt (0.1Ω) with 10mA LSB: CAL = 0.00512 / (0.01 * 0.1) = 51.2"*.
Two things are wrong with that line: `0.00512` is the **INA226's** scaling constant — the
INA219's is `0.04096` — and the arithmetic as written gives 5.12, not 51.2.

For a true 10 mA/bit on 0.1 Ω the register should have been **41**.

Nothing about this was visible from the device: the readings were self-consistent, stable
and plausible, just uniformly high. `test/` asserts the whole story.

## Calibration

```
current_lsb = max_current_a / 32768
CAL         = 0.04096 / (current_lsb * shunt_ohms)      /* 0.04096, not 0.00512 */
power_lsb   = 20 * current_lsb                          /* 20, not the INA226's 25 */
```

`report.current_lsb_a` is derived **from the rounded register**, not from what was
requested, and `ina219_read()` scales by that. Scaling by the requested value is exactly
the mistake above.

## No ID registers

Unlike the INA226 there is nothing to identify the part with, so `create()` writes the
calibration and reads it back. An address that acknowledges but is something else will not
reproduce it — the alternative is returning zeros forever.

## Tests

```sh
make -C test
```
