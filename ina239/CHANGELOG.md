# Changelog

All notable changes to this component are documented here.

## [0.1.0] - 2026-09-06

Initial release. Written against SLYS027A (Rev. A, May 2022).

### Added

- Handle-based API over a caller-supplied `spi_device_handle_t`, matching the
  I2C drivers in this repository: creatable before a bus exists, re-pointable
  with `ina239_set_device()`, and never the owner of the transport.
- `ina239_device_config()`, which fills an `spi_device_interface_config_t` with
  the part's own clock mode. SPI mode is silicon, not board: a mode-0 handle
  does not fail, it returns plausible, stable, wrong numbers.
- `ina239_configure()`: a two-step identity check, the ADC range, averaging and
  SHUNT_CAL, with an `ina239_stage_t` naming which of the six steps failed and
  the observed MANUFACTURER_ID / DEVICE_ID when the part is not an INA239.
  DEVICE_ID is checked as well as MANUFACTURER_ID because an INA228, INA237 or
  INA238 answers "TI" and has a different shunt LSB — it would read wrong by a
  factor of four with nothing looking amiss.
- `ina239_read()`: bus voltage, shunt voltage, die temperature, current and
  power in one call, with MEMSTAT, MATHOF and CNVRF decoded into separate health
  facts rather than one opaque register.
- `config_intact`, which reads CONFIG and ADC_CONFIG back alongside the
  measurements. A part that browned out and reset goes on answering — on the
  wide range, so every current on a 41 mV board is a quarter of the truth.
  `shunt_cal_matches` cannot see that: SHUNT_CAL's reset value is 1000h, which
  is 4096, the exact constant the calibration identity produces. The remaining
  blind spot (a caller asking for the reset defaults) is documented and pinned
  by a test.
- A host test suite: the SPI frame shape, both identity refusals, sign
  extension of VSHUNT and CURRENT, DIETEMP's signed 12 bits in 15:4, the 24-bit
  POWER assembly, Equation 4, and the claim that one constant SHUNT_CAL of 4096
  is correct for every shunt in both ADC ranges.
