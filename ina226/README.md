# ina226

TI INA226 bidirectional current, voltage and power monitor over I2C.

## Installing

```yaml
# your project's idf_component.yml
dependencies:
  ina226:
    git: https://github.com/benvonhandorf/esp_components.git
    path: ina226
    version: ina226-v0.2.0
```

## Using it

```c
ina226_config_t cfg = {
    .dev           = i2c_device_handle(INA226_I2C_ADDR_DEFAULT),
    .shunt_ohms    = 0.01f,     /* the resistor actually fitted */
    .max_current_a = 8.192f,    /* the largest current to measure */
    .averaging     = INA226_AVG_16,
};

ina226_handle_t ina;
ina226_report_t report;
if (ina226_create(&cfg, &ina, &report) != ESP_OK) {
    printf("INA226 %s failed\n", ina226_stage_name(report.failed_stage));
}

ina226_reading_t r;
ina226_read(ina, &r);   /* r.bus_voltage, r.current, r.power, r.shunt_voltage */
```

## Calibration is derived, not hardcoded

`shunt_ohms` and `max_current_a` determine the resolution:

```
current_lsb = max_current_a / 32768        (Eq. 2; the current register is signed 15-bit)
CAL         = trunc(0.00512 / (current_lsb * shunt_ohms))       (Eq. 1)
power_lsb   = 25 * current_lsb                                  (25, not the INA219's 20)
```

Ask for more range than you need and you throw away resolution; ask for less and
readings saturate. The report says what you actually got — `current_lsb_a` and
`full_scale_a` are rarely the round numbers requested.

`current_lsb_a` is derived **from the calibration register**, not from what was
requested, and `ina226_read()` scales by that. The two differ whenever the calibration
does not come out whole, and scaling by the requested value puts that difference into
every reading.

The arithmetic is host-tested, because getting it wrong does not *fail*: it produces
readings that look entirely reasonable and are wrong by a constant factor.

## Ranges it will refuse

The shunt input is fixed at **±81.92 mV** — unlike the INA219 there is no PGA to widen
it — and the calibration register is **fifteen bits** (Table 7-11 names it FS14:FS0 and
leaves D15 unnamed). Both ends return `ESP_ERR_INA226_BAD_RANGE`:

| Request | Full-scale shunt drop | |
|---|---|---|
| 32.768 A on 0.01 Ω | 328 mV | past the input; everything above 8.192 A would read as 8.192 A |
| 8.192 A on 0.01 Ω | 81.92 mV | accepted — exactly the ceiling, at 250 µA/bit |
| 51.2 mA on 0.1 Ω | 5.12 mV | too little; CAL would be 32768, one past the register |

`CAL = 167.772 / V_fullscale`, so the usable calibration range is 2048 to 32767. A
calibration below 2048 means the current register spans more than the ADC can deliver,
which only throws away resolution.

## Identifying the part

The die ID register is `DID[15:4]` plus `RID[3:0]`, the die revision — and the register
map lists **both 2260h and 2261h** as an INA226 (*"Die COO: 2260 = USA or Japan, 2261 =
USA"*). Only the device half is compared, so a part off either line is accepted.

## It does not own the bus

`cfg.dev` is a device handle the caller created and owns. It **may be NULL** — the handle
is still created, and every call needing the bus returns `ESP_ERR_INVALID_STATE` until
`ina226_set_device()` supplies one. That allows a handle to exist before anything is
powered.

A bus that is torn down and rebuilt invalidates every device handle taken from it, so
whoever owns the bus must call `ina226_set_device()` again rather than this driver caching
a handle that can dangle.

## Identification before configuration

`create()` reads the manufacturer and die IDs first and returns
`ESP_ERR_INA226_WRONG_PART` if they are not TI's INA226. Writing a reset to whatever
happens to answer at 0x40 — an INA219, an EEPROM — is worse than saying the part is not
what was expected.

## Averaging

`cfg.averaging` is written straight into the AVG field, so a zero-initialised config gets
`INA226_AVG_1` — **no averaging**. The reference design used `INA226_AVG_16`; ask for it
explicitly.

## The ALERT pin

`ina226_clear_alert()` clears the flags; call it from whatever services the pin. This
driver registers no interrupt handler: which pin the part is wired to, and how interrupts
are dispatched, are facts about the board, not about the part.

## Tests

```sh
make -C test
```
