# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-05

Initial release, extracted from `solar_power_monitor`'s `components/aw9523b_driver`.

### Changed from the original

- **Instances, not a singleton**; the caller owns the I2C device.
- **Pin directions are configuration, not constants.** The original hardcoded `P0_CONFIG
  = 0xF0` and `P1_CONFIG = 0xFF` for one board's relay wiring.
- **Output levels are established before direction**, which the original did for port 0
  only. Switching direction first lets whatever the output register held reach the pins
  -- an audible clack on a board driving relays, and worse on one driving FETs.
- **The part is identified** by its ID register before anything is written.
- **`set_pin()` uses a shadow of the output register.** Reading a port returns the
  *input* register, so a read-modify-write through it writes input levels back over the
  outputs.
- **Push-pull on port 0 is configurable.** It is open-drain by default, which surprises
  anyone expecting it to drive a load high.
- **`aw9523b_on_interrupt(void)` became `aw9523b_clear_interrupt(handle)`.**

### Not yet verified on hardware

The register sequences are unchanged from code that ran on an M5Stack StamPLC. The
instance rework has not been run against a part.
