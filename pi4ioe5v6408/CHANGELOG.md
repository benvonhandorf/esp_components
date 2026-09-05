# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-05

Initial release, extracted from `solar_power_monitor`'s `components/pi4ioe5v6408_driver`.

### Changed from the original

- **Instances, not a singleton**; the caller owns the I2C device.
- **The API is about pins, not about one board's peripherals.** It was
  `set_backlight()`, `set_led(led, on)` and `read_buttons()`, with the LED polarity and
  the button-to-pin mapping of a single product baked in. Those names belong to board
  code.
- **Direction, pulls and interrupt-on-change are configuration**, not constants.
- **Output levels are established before direction.**
- **The part is identified** from the device ID in its control register.
- **`set_pin()` uses a shadow of the output register**, since the input register
  reflects pin state rather than what was last driven.
- **`pi4ioe5v6408_on_interrupt(void)` became `read_interrupt_status(handle,
  &changed)`**, which says which pins changed instead of only acknowledging.

### Not yet verified on hardware

The register sequences are unchanged from code that ran on an M5Stack StamPLC. The
instance rework has not been run against a part.
