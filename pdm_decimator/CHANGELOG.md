# Changelog

All notable changes to this component are documented here.

## [0.1.0] - 2026-10-16

Initial release, extracted from the `esp_board_bringup` rig, where the filters
had been written for one SPH0641LU4H-1 microphone pair and were tied to that
project's headers.

- `pdm_decimator_create()` and friends: one handle type for both filters, any
  channel count from 1 to 8, with the filter, ratio, bit order and whether to use
  the vector unit chosen in a config struct.
- `PDM_DECIMATOR_SINC6`: sixth-order sinc by 16, 32, 48 or 64, table-driven, plain
  C. Unchanged arithmetic.
- `PDM_DECIMATOR_TWO_STAGE`: sinc^4 by 8 over bytes then a 24-tap FIR by 2, fixed
  4.8 MHz to 300 kHz. The FIR is split into `pdm_stage2_scalar.c` (portable, and
  the reference) and `pdm_stage2_simd.c` (ESP32-S3 vector unit, built only for
  that target).
- Added: `lsb_first` for the two-stage filter, which could only take MSB-first
  words before.
- Added: `pdm_decimator_reserve()`, so the two-stage work buffer can be allocated
  before the receive path instead of on its first call.
- Errors are `esp_err_t` rather than a NULL pointer or a zero count, with
  `PDM_DECIMATOR_ERR_BAD_RATIO` and `PDM_DECIMATOR_ERR_BAD_CHANNELS` (base
  `0x3F000`).
- Built `-O2` by the component itself, so a project at `-Og` does not overrun its
  receiver.

### Fixed relative to the code it came from

- The two-stage filter dereferenced a NULL work buffer when its first call
  carried no frames.

### Not done

- The capture statistics (`audio_stats16_*`) stayed in the application. They
  summarise PCM and are not specific to PDM.
- More than two channels is tested on the host and against both filters' own
  arithmetic. Whether the ESP32-S3 I2S driver delivers more than two raw slots
  per frame in the order a caller assumes has not been checked on hardware.
