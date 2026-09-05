# Changelog

All notable changes to this component are documented here.

## [Unreleased]

Validated line by line against SBOSA20A (Rev. A, May 2022). No defects found:
Equation 1 reproduces the datasheet's own design example (SHUNT_CAL 4050 for a
16.2 mOhm shunt at 305.1758 uA/bit), the decode constants match Tables 7-9
through 7-12, and leaving CONFIG and ADC_CONFIG at their reset values is exactly
what section 8.2.2.2 prescribes.

### Added

- A host test suite. The component had none, and its central claim -- that one
  constant SHUNT_CAL of 4096 is correct for every shunt -- is an algebraic
  identity worth pinning: choosing `CURRENT_LSB = 5 uV / RSHUNT` makes RSHUNT
  cancel out of Equation 1. That is now checked across shunts from 0.5 mOhm to
  1 ohm, alongside the datasheet's design example.
- A fake I2C device (`test/fake_i2c.c`) and host stubs for the two ESP-IDF
  headers the public API needs, so the decode path is testable off-target: sign
  extension of VSHUNT and CURRENT, DIETEMP's signed 12 bits in 15:4 with its
  reserved low nibble, the 24-bit POWER assembly, Equation 4, and the three
  health flags.

### Changed

- Moved from `esp_board_bringup/components/` into the shared `esp_components`
  repository, so it can be depended on without pulling in a bring-up application.
  Only the repository metadata changed; the driver, its comments and its interface
  are untouched.

## [0.1.0] - 2026-08-31

Initial release, extracted from the `esp_board_bringup` console application
where the driver had grown up fused to a command-line interface.

- Handle-based API over a caller-supplied `i2c_master_dev_handle_t`.
- `ina237_configure()`: MANUFACTURER_ID probe and SHUNT_CAL programming, with
  an `ina237_stage_t` naming which of the two failed and the observed
  manufacturer ID when the part is not an INA237.
- `ina237_read()`: bus voltage, shunt voltage, die temperature, current and
  power in one call, with MEMSTAT, MATHOF and the SHUNT_CAL read-back decoded
  into three separate health facts rather than one opaque register.
