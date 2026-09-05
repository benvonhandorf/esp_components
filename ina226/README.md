# ina226

TI INA226 bidirectional current, voltage and power monitor over I2C.

## Installing

```yaml
# your project's idf_component.yml
dependencies:
  ina226:
    git: https://github.com/benvonhandorf/esp_components.git
    path: ina226
    version: ina226-v0.1.0
```

## Using it

```c
ina226_config_t cfg = {
    .dev           = i2c_device_handle(INA226_I2C_ADDR_DEFAULT),
    .shunt_ohms    = 0.01f,     /* the resistor actually fitted */
    .max_current_a = 32.768f,   /* the largest current to measure */
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
current_lsb = max_current_a / 32768        (the current register is signed 15-bit)
CAL         = 0.00512 / (current_lsb * shunt_ohms)
```

Ask for more range than you need and you throw away resolution; ask for less and
readings saturate. The report says what you actually got — `current_lsb_a` and
`full_scale_a` are rarely the round numbers requested.

The arithmetic is host-tested, because getting it wrong does not *fail*: it produces
readings that look entirely reasonable and are wrong by a constant factor.

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

## The ALERT pin

`ina226_clear_alert()` clears the flags; call it from whatever services the pin. This
driver registers no interrupt handler: which pin the part is wired to, and how interrupts
are dispatched, are facts about the board, not about the part.

## Tests

```sh
make -C test
```
