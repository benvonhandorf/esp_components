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
where the driver had grown up fused to a command-line interface. Before the
split there was no device API at all -- the whole power-up sequence lived
inside the `init` command.

- Handle-based API over a pair of GPIOs, with `hx711_bring_up()` carrying the
  power-cycle, mode selection, rate measurement and proof batch behind one call
  and an `hx711_stage_t` naming which of them failed.
- Cycle-accurate clock bursts in IRAM, with each PD_SCK high phase timed
  individually against the data sheet's 50 us T3 limit and the reading
  discarded when one exceeds it.
- Output-rate measurement against a time budget, and `hx711_rate_classify()`
  for the Table 2 bands.
- Averaged reads with Welford variance, standard error of the mean, saturation
  detection and an all-identical check that fails rather than warns.
- Tare, calibration against a known mass with a standard-error guard, and
  weighing in the resulting units.
- `hx711_set_scale()` and `hx711_bringup_opts_t.set_scale`, so a factor derived
  once on a bench can be compiled into a consumer.

### Fixed relative to the pre-extraction code

- Re-running the equivalent of `init` with different pins used to overwrite the
  stored pin numbers without releasing the previous claim, abandoning the old
  pair. A new pin pair is now a new handle, so the old one is released.

### Known, and deliberately carried across unchanged

- The saturation check tests against `HX711_FULL_SCALE - 1`, the exact end
  code. `components/nau7802` measured that this effectively never fires and
  uses 0.99 of full scale instead; this driver should follow, but the change is
  behavioural and did not belong in the extraction.
- `src/hx711.c` includes `esp_private/esp_clk.h` for one call. See the
  Portability section of `README.md`.
