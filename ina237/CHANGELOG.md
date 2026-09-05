# Changelog

All notable changes to this component are documented here.

## [Unreleased]

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
