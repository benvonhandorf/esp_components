# ina239

Driver for the TI INA239, an 85 V current, voltage and power monitor that
measures across an external shunt and speaks **SPI**. Identity, ADC range and
averaging, shunt calibration, the full measurement set, and the health bits that
say when a reading cannot be trusted.

It is the SPI sibling of the INA238: the same measurement set and the same
arithmetic over a different transport. There is no address — a bus carries as
many of these as it has chip selects — and every frame carries the register, so
there is no burst read.

## Installing

```yaml
# your project's idf_component.yml
dependencies:
  ina239:
    git: https://github.com/benvonhandorf/esp_components.git
    path: ina239
    version: ina239-v0.1.0
```

## Using it

The driver does not own the SPI bus and does not add a device to it. You pass in
a device handle you made, and you keep owning it — so the same bus can carry
other parts, managed however your application already manages them.

The one thing it will not leave to you is the clock mode. Mode is a property of
the silicon, and a mode-0 handle does not fail: it returns numbers that are
plausible, stable and wrong. So `ina239_device_config()` fills the struct and
you add it to your bus:

```c
#include "ina239.h"

spi_device_interface_config_t dev_cfg = ina239_device_config(CS_GPIO, 1000000);
spi_device_handle_t dev;
ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev));

const ina239_config_t cfg = {
    .dev        = dev,
    .shunt_ohms = 0.010,               /* the resistor actually fitted */
    .range      = INA239_RANGE_41MV,   /* 1.25 uV/LSB, if the shunt was sized for it */
    .averaging  = INA239_AVG_16,
};

ina239_handle_t ina;
ESP_ERROR_CHECK(ina239_create(&cfg, &ina));

ina239_config_report_t report;
esp_err_t err = ina239_configure(ina, &cfg, &report);
if (err == ESP_ERR_INA239_WRONG_DEVICE) {
    /* A TI part answered, but DEVICE_ID said report.die_id, not 239h. */
}

ina239_reading_t r;
if (ina239_read(ina, &r) == ESP_OK) {
    printf("%.3f V  %.4f A  %.3f W  %.1f C\n",
           r.bus_v, r.current_a, r.power_w, r.temp_c);
}
```

If your SPI layer caches device handles, call `ina239_set_device()` before each
use. A cache that recycles entries, or a bus that is torn down and rebuilt,
leaves a previously held handle dangling.

## It returns facts, not text

Every call that can fail interestingly fills an out-struct naming *which stage*
failed and what was observed there, and the caller decides how to say so.
`ina239_configure()` walks five steps — probe, identify, device id, CONFIG,
ADC_CONFIG, SHUNT_CAL — and a bare "configure failed" would send someone looking
at the wrong half of the board.

## Things worth knowing

**Identity is checked twice, on purpose.** An SPI chip select with nothing on it
reads back a plausible `0x0000` or `0xFFFF` rather than failing, so a probe that
only checked the transfer would report success against thin air —
`MANUFACTURER_ID` catches that. `DEVICE_ID` then catches the more likely
mistake: an INA228, INA237 or INA238 answers "TI" just as happily, and its shunt
LSB differs, so its currents would be wrong by a factor of four with nothing
looking amiss.

**One SHUNT_CAL constant covers every shunt, in both ranges.** Choosing
`CURRENT_LSB = shunt LSB / RSHUNT` — the value that makes full-scale current
exactly fill the CURRENT register — cancels RSHUNT out of Equation 1 entirely.
It cancels the range too, because dropping the LSB from 5 µV to 1.25 µV divides
the product by four and the ADCRANGE = 1 rule multiplies it straight back.
Both arrive at 4096. There is a host test that checks this across shunts from
0.5 mΩ to 1 Ω in both ranges, because it is the kind of identity that is easy to
believe and expensive to be wrong about.

**A reset part goes on answering.** It answers on the wide range, so on a board
configured for 41 mV every current it reports is a quarter of the truth — a
failure that looks exactly like a real measurement. `ina239_read()` therefore
reads CONFIG and ADC_CONFIG back with the measurements and reports
`config_intact`.

`shunt_cal_matches` on its own cannot do this job, and it is worth saying why:
SHUNT_CAL's reset value is `1000h`, which is 4096 — the very constant the
identity above produces. The register is correct after a reset. CONFIG and
ADC_CONFIG close the gap except in one case, which the header states and a test
pins: a caller that asks for the wide range *and* no averaging is asking for
exactly the reset defaults, and then nothing distinguishes a reset part from a
configured one. Ask for averaging, or for the 41 mV range, and `config_intact`
means what it says.

**ADCRANGE is written, unlike the I2C drivers here.** `ina237` leaves CONFIG
alone; this one does not. The range is half of what a shunt choice buys, and a
board whose shunt was sized for the full 41 mV span loses two bits by being left
at the reset default.

**`ina239_read_register()` reads 16 bits.** That covers every register but
POWER, which is 24. Use `ina239_read()` for power; the register map returned by
`ina239_register_map()` carries each register's width so a diagnostic dump can
say which one it truncated.

## Testing

```sh
make -C test
```

No hardware and no ESP-IDF: `test/stub/` supplies just enough of `esp_err.h`,
`driver/spi_master.h` and `esp_rom_sys.h` to build the driver, and
`test/fake_spi.c` answers its transfers from a register map.

The fake decodes the command byte the way the *datasheet* describes it —
address in bits 7:2, reserved bit 1 low, R/W in bit 0 — rather than reusing the
driver's own `CMD_READ`/`CMD_WRITE` macros. That is deliberate: those macros are
what is under test, and an address shifted one bit too few still round-trips
cleanly through a fake that shares them.

## Reference

INA239 datasheet, SLYS027A (January 2021, revised May 2022). Section references
appear in the source where a constant is not self-evident.
