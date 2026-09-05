# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.2.0] - 2026-09-05

Validated line by line against SBOS547C (Rev. C, August 2026). The scaling constants
(0.00512, 25x), the shunt and bus LSBs and the configuration word were all correct, and
Equations 1, 3 and 4 reproduce the datasheet's Table 6-1 end to end. Five things were not.

### Fixed

- **Readings were scaled by the requested current LSB, not the one the calibration
  register yields.** `current_lsb_a` was set from `max_current_a / 2^15` before the
  calibration was rounded, and `ina226_read()` scaled by it -- a systematic error in every
  current and power value whenever the calibration did not come out whole (measured
  -0.007% to -0.043% across ordinary shunt and range choices). This is the same defect the
  sibling INA219 driver was written to fix; the LSB is now derived from the register.

- **The calibration register is fifteen bits, but 65535 was accepted.** Table 7-11 names
  the field FS14:FS0 and leaves D15 unnamed, so the largest value the part can hold is
  32767. Since `CAL = 0.00512 x 2^15 / V_fullscale`, anything needing under about 5.12 mV
  of full-scale shunt drop overflowed: 0.1 ohm at 30 mA computed CAL 55924, of which the
  part keeps 23156, rescaling every reading by **2.42x**. Such a range is now refused.

- **Nothing checked the fixed +/-81.92 mV shunt input range.** There is no PGA on this part
  to widen it. The configuration this component was extracted with -- 0.01 ohm at
  32.768 A -- develops 328 mV and is four times beyond it: everything above 8.192 A read
  as 8.192 A while `full_scale_a` reported 32.768 A. Using the whole shunt range instead
  (8.192 A) also recovers two bits of resolution, 250 uA/bit rather than 1 mA/bit. Ranges
  past the ceiling are now refused.

- **The die ID check rejected genuine parts.** Table 7-15 splits the register into
  DID15:4 and RID3:0, the die revision, and the register map lists both 2260h and 2261h as
  an INA226 ("Die COO: 2260 = USA or Japan, 2261 = USA"). Comparing all sixteen bits
  against 2260h turned a part off the other line into `ESP_ERR_INA226_WRONG_PART`. Only
  the device half is compared now.

- **The documented averaging default did not exist.** The header said "0 selects
  INA226_AVG_16, which is what the reference design used", but `INA226_AVG_1 = 0` and
  `configure()` writes the field unmapped, so a zero-initialised config got no averaging
  at all -- sixteen times noisier than the comment promised. The documentation now matches
  the code; `INA226_AVG_16` has to be asked for.

### Changed

- **Equation 1 is truncated rather than rounded.** The old comment defended rounding on
  the grounds that truncation "biases every reading low", which was only true because the
  requested LSB was being reported; with the LSB derived from the register there is no
  bias either way. Truncating guarantees `full_scale_a` covers the range that was asked
  for. A quotient sitting within float rounding error of an integer is snapped up onto it,
  so the datasheet's own CAL 2560 does not come out 2559.

### Added

- Tests for all five: the reported LSB against the register's own inverse, the fifteen-bit
  limit and the 2.42x overflow beyond it, the +/-81.92 mV ceiling at both ends, die IDs
  across revisions, and the datasheet's Table 6-1 worked example end to end.

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
