# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-05

Initial release, extracted from `solar_power_monitor`'s `components/lm75bdp_driver`.

### Changed from the original

- **Instances, not a singleton**: state moved into an opaque `lm75bdp_handle_t`.
- **The caller owns the I2C device**, and can re-point the driver after a bus rebuild.
- **A handle is creatable before there is a bus.**
- **Thresholds round rather than truncate.** The original computed `(int16_t)(temp_C *
  2.0f)`, which moves toward zero: -0.7 C became -0.5 rather than -1.0, putting the
  threshold on the wrong side of the temperature it was meant to catch -- and only below
  freezing, so a bench test at room temperature would never show it.
- **Over-range thresholds clamp** instead of being masked into the 9-bit field, which
  would wrap a hot limit into a cold one.
- **The programmed thresholds are reported back**, since both are quantised to 0.5 C.
- **The part is confirmed by configuration read-back**, as it has no ID register.
- **`lm75bdp_on_interrupt(void)` is gone.** Which pin the OS output is wired to is a
  fact about the board.

### Not yet verified on hardware

The register sequences are unchanged from code that ran on an M5Stack StamPLC. The
instance rework and the arithmetic fixes have host tests, but neither has been run against a part.
