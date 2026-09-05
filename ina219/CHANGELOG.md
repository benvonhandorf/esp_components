# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.2.0] - 2026-09-05

Validated line by line against SBOS448G (Rev. G, December 2015). The scaling constants,
the register formats and the configuration word were all correct — the configuration the
driver writes, `0x399F`, is bit-identical to the documented power-on default. The
calibration register's void low bit was not accounted for.

### Fixed

- **The calibration register's low bit is void, so about half of all valid configurations
  were rejected as the wrong part.** Figure 27 types FS0 as `R-0` where FS15:FS1 are
  `R/W-0`, with the note: *"FS0 is a void bit and will always be 0. It is not possible to
  write a 1 to FS0. CALIBRATION is the value stored in FS15:FS1."* `create()` confirms the
  part by reading the calibration register back, and an odd value never compares equal —
  3331 of 6759 in-range shunt and range combinations, among them the datasheet's own
  2 mΩ / 15 A design example (CAL 44739). Calibrations are now masked even before they are
  written, so the read-back matches and the reported resolution is the one the part uses.

- **A range beyond the PGA's reach was accepted and then misreported.** The driver
  programs PGA /8, which sees 320 mV across the shunt and no more. 10 A on a 0.1 Ω shunt
  was accepted, and `full_scale_a` reported 10 A, but everything above 3.2 A reads as
  3.2 A. Such a request is now refused with `ESP_ERR_INA219_BAD_RANGE`.

- **Equation 1 truncates; the code rounded.** Rounding up shrinks the current LSB and pulls
  full scale below the range that was asked for — 585 of 1980 swept cases. It now
  truncates, so the requested range stays reachable. A quotient sitting within float
  rounding error of an integer is snapped up onto it first, so a range whose calibration
  is exactly representable does not come out one short.

- **A calibration of 1 was accepted.** Masked for FS0 it stores as 0, and a zero
  calibration leaves the current and power registers reading zero forever (Table 2,
  note 2). The floor is now 2.

- **`ina219_clear_alert()` documented hardware this part does not have.** The header
  described clearing flags "by reading the mask/enable register", called from whatever
  services the ALERT pin. The INA219 has neither: its register map is six registers,
  00h–05h, and its pinout is IN+, IN−, GND, VS, SCL, SDA, A0, A1. That is the INA226. The
  implementation was already a no-op and said so.

### Changed

- **0.1.0's 24.5% figure was itself computed without FS0.** The original wrote CAL 51,
  which the part stores as 50, so its true resolution was 8.192 mA/bit rather than the
  8.03 mA/bit that dividing by 51 suggests, and its readings were 10 / 8.192 = **1.221×**
  the truth. Replacing the old driver changes current and power by about **22%**, not
  24.5%. For the same reason, "the register should have been 41" was wrong twice over: 41
  stores as 40, and 10 mA/bit on a 0.1 Ω shunt is 327.68 A of full scale — 32.8 V across
  the shunt, which no PGA setting can see.

### Added

- Tests for the void FS0 bit: that every calibration produced survives it, that the
  reported LSB is the one the stored value yields, that the inverse models it too, and
  that the range guards refuse what the PGA cannot reach. The datasheet's Table 8 worked
  example is now asserted end to end.

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
