# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-05

Initial release, extracted from `solar_power_monitor`'s `components/rx8130ce_driver`.

### Changed from the original

- **Instances, not a singleton**; the caller owns the I2C device.
- **A drained clock is reported, not returned.** Registers that do not decode to a valid
  date now give `ESP_ERR_RX8130CE_NOT_SET` rather than a plausible-looking wrong time,
  and the part's voltage-low flags are surfaced as `power_lost`.
- **`timegm()`, not `mktime()`.** The RTC holds UTC; `mktime()` applies whatever
  timezone happens to be set and shifts the result by it.
- **Setting the time clears the voltage-low flags**, so a clock that has been set stops
  reporting itself unreliable.
- **Years outside 2000-2099 are refused** rather than silently wrapping the two-digit
  register.
- **`rx8130ce_on_interrupt(void)` is gone.**

### Not yet verified on hardware

The register sequences are unchanged from code that ran on an M5Stack StamPLC. The
instance rework and the arithmetic fixes have host tests, but neither has been run against a part.
