# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-05

Initial release, extracted from `solar_power_monitor`'s
`components/ina226_driver`.

### Changed from the original

- **Instances, not a singleton.** The device handle was a file static, so two parts on
  one board were impossible. State moved into an opaque `ina226_handle_t`.
- **The caller owns the I2C device.** The original took a bus handle and created its own
  device. The bus owner also owns the device cache and its lifetime, so the driver now
  takes a device handle and can be re-pointed at a new one after a bus rebuild.
- **A handle is creatable before there is a bus.** `cfg.dev` may be NULL; calls needing
  the bus return `ESP_ERR_INVALID_STATE` until one arrives.
- **Calibration is computed from the shunt.** It was `#define INA226_CALIBRATION_DEFAULT
  512`, correct only for one board's 0.01 ohm shunt. Deriving it from `shunt_ohms` and
  `max_current_a` reproduces exactly 512 for that board — asserted in the host test, so
  the rework provably did not change what the original device measured.
- **The part is identified before it is configured**, rather than resetting whatever
  answers at the address.
- **Failures name the stage** they happened at.
- **The bus voltage register is read unsigned.** The original cast it to `int16_t`, which
  works only because the part's 36 V ceiling lands below 0x7FFF; at 0x8000 a signed read
  reports -40.96 V.
- **`ina226_on_interrupt(void)` became `ina226_clear_alert(handle)`.** The old name and
  signature assumed a particular project's interrupt dispatcher.

### Not yet verified on hardware

The register sequences are unchanged from code that ran on a StamPLC, and the calibration
is host-tested against the original constant. The instance rework itself has not been run
against a part.
