# Changelog

All notable changes to this component are documented here.

## [Unreleased]

### Changed

- Moved from `esp_board_bringup/components/` into the shared `esp_components`
  repository, so it can be depended on without pulling in a bring-up application.
  Only the repository metadata changed; the driver, its comments and its interface
  are untouched.

## [0.2.0] - 2026-08-31

### Added

- `nau7802_set_scale()`, and `set_scale` / `counts_per_unit` in
  `nau7802_bringup_opts_t`, so a scale factor derived once on a bench can be
  compiled into a consumer rather than re-measured on every boot. The tare is
  deliberately not part of this: it is the bridge's own zero and moves with
  temperature and mounting, so it stays a runtime measurement.
- `nau7802_scale_t.supplied` and `nau7802_change_report_t.scale_was_supplied`,
  which distinguish a supplied factor from a measured one. A gain or channel
  change drops both -- a factor is counts per unit at one gain -- but the advice
  a caller should give differs, and the provenance cannot be recovered after the
  reset.
- `nau7802_weight_t.tare_taken`. `nau7802_calibrate()` has always refused
  without a tare of at least two samples, so `calibrated` used to imply a real
  zero; a supplied factor breaks that, and an untared reading is the bridge's
  own offset reported as load.

## [0.1.0] - 2026-08-30

Initial release, extracted from the `esp_board_bringup` console application
where the driver had grown up fused to a command-line interface.

- Handle-based API over a caller-supplied `i2c_master_dev_handle_t`.
- Power-up sequence per data sheet section 9.1, including `REG0x15 = 0x30` and
  the VLDO-before-AVDDS ordering.
- DRDY interrupt support, falling back to polling `PU_CTRL.CR`.
- Averaged reads with Welford variance, standard error of the mean, and
  saturation detection.
- Configuration setters that recalibrate, restart and flush as the analog path
  requires, reporting what that cost.
- Tare, calibration against a known mass with a standard-error guard, and
  weighing in the resulting units.
