# ina219

TI INA219 bidirectional current, voltage and power monitor over I2C.

## Installing

```yaml
dependencies:
  ina219:
    git: https://github.com/benvonhandorf/esp_components.git
    path: ina219
    version: ina219-v0.2.0
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

**Current and power readings will change by about 22%.**

The driver this replaces wrote a calibration register of 51 for a 0.1 Ω shunt and then
scaled every current reading by 10 mA/bit. Those two do not agree. 51 is odd, and the
calibration register's low bit cannot be written (see below), so the part actually stored
**50** — a resolution of **8.192 mA/bit**. Readings were therefore 10 / 8.192 = 1.221×
the true value.

Its comment read *"R100 shunt (0.1Ω) with 10mA LSB: CAL = 0.00512 / (0.01 * 0.1) = 51.2"*.
Two things are wrong with that line: `0.00512` is the **INA226's** scaling constant — the
INA219's is `0.04096` — and the arithmetic as written gives 5.12, not 51.2.

The 10 mA/bit it was aiming for was not reachable either. Over a signed 15-bit register
that is 327.68 A of full scale, which across 0.1 Ω is 32.8 V of shunt drop against a
320 mV input. The practical cost was resolution: 3.2 A at 8.192 mA/bit uses 390 of the
register's 32768 codes, throwing away better than six bits.

Nothing about this was visible from the device: the readings were self-consistent, stable
and plausible, just uniformly high. `test/` asserts the whole story.

## Calibration

```
current_lsb = max_current_a / 32768                      /* Eq. 2 */
CAL         = trunc(0.04096 / (current_lsb * shunt_ohms)) & 0xFFFE
                                                         /* Eq. 1; 0.04096, not 0.00512 */
power_lsb   = 20 * current_lsb                           /* Eq. 3; not the INA226's 25 */
```

`report.current_lsb_a` is derived **from the register as the part stores it**, not from
what was requested, and `ina219_read()` scales by that. Scaling by the requested value is
exactly the mistake above.

Equation 1 truncates rather than rounds, so the current LSB never comes out smaller than
asked for and the requested range stays reachable.

## The calibration register's low bit is void

Datasheet Figure 27 types FS0 as `R-0` where FS15:FS1 are `R/W-0`:

> FS0 is a void bit and will always be 0. It is not possible to write a 1 to FS0.
> CALIBRATION is the value stored in FS15:FS1.

So an odd calibration is silently stored as one less. Two things depend on honouring that,
and both are why the mask is in the formula above: the read-back used to identify the part
would not match what was written — about half of all otherwise valid shunt and range
combinations, including the datasheet's own 2 mΩ / 15 A design example — and the current
LSB derived from the written value would not be the one the part uses.

## Ranges it will refuse

`max_current_a × shunt_ohms` has to land inside the PGA's window, and the calibration has
to fit 16 bits. Both ends return `ESP_ERR_INA219_BAD_RANGE` with
`report.failed_stage == INA219_STAGE_RANGE`:

| Request | Shunt drop at full scale | |
|---|---|---|
| 10 A on 0.1 Ω | 1 V | past the PGA's 320 mV; everything above 3.2 A would read as 3.2 A |
| 3.2 A on 0.1 Ω | 320 mV | accepted — exactly the ceiling |
| 0.2 A on 0.1 Ω | 20 mV | too little; CAL would exceed 65535 |

The driver programs PGA /8, the widest of the four gain settings, so 320 mV is the most it
can see whatever the calibration says.

## No ID registers

Unlike the INA226 there is nothing to identify the part with, so `create()` writes the
calibration and reads it back. An address that acknowledges but is something else will not
reproduce it — the alternative is returning zeros forever.

## Tests

```sh
make -C test
```
