# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-05

Initial release, extracted from `solar_power_monitor`'s `components/ina219_driver`.

### Fixed

- **Current and power were about 24.5% high.** The calibration register was set to 51
  while the read path scaled by 10 mA/bit; CAL = 51 on the board's 0.1 ohm shunt actually
  yields 8.03 mA/bit. The originating comment cited `0.00512` — the INA226's scaling
  constant, not the INA219's `0.04096` — and its arithmetic gave 5.12 rather than the 51.2
  it claimed. For a true 10 mA/bit the register should have been 41.

  Readings from a device using the old driver were self-consistent and plausible, so this
  was invisible without doing the arithmetic. It is now asserted in `test/`.

### Changed from the original

- **Instances, not a singleton**: state moved into an opaque `ina219_handle_t`, so two
  parts can coexist.
- **The caller owns the I2C device**, and can re-point the driver at a new one after a bus
  rebuild rather than the driver caching a handle that can dangle.
- **A handle is creatable before there is a bus**; calls needing it return
  `ESP_ERR_INVALID_STATE`.
- **Calibration is computed from the shunt and range**, and the reported resolution is
  derived from the rounded register rather than from the request.
- **The part is confirmed by calibration read-back**, since the INA219 has no ID register.
- **Failures name the stage** they happened at.

### Not yet verified on hardware

The register sequences are unchanged apart from the calibration value, which is now
correct. The instance rework has not been run against a part.
