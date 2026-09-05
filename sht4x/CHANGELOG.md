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
- Write-wait-read command sequencing, which this part requires: a
  repeated-start write-then-read makes it NACK the read header.
- Measurement at all three repeatabilities, the six implemented heater
  settings, serial number and soft reset.
- CRC-8 checking that reports *which* word failed and what was computed for
  it, rather than a bare failure.
- Humidity reported both as converted and cropped to the physical 0-100 %RH
  range, with a flag saying whether cropping happened.
