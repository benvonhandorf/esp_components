# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- Output fan-out (`diag_printf`, `diag_vprintf`, `diag_write`, `diag_error`) with a
  registerable sink table and optional `ESP_LOGx` capture.

Extracted from `esp_board_bringup`'s `main/console/output.c`, where it was part of the
console. Split out because the fan-out -- not the shell -- is what makes logging to a
filesystem, to MQTT or to a web page possible, and a headless project wants it without
a REPL.

### Changed from the original

- `bp_*` names become `diag_*`.
- `bp_output_init(void)` becomes `diag_init(const diag_config_t *)`, so a project can
  decline the `ESP_LOGx` redirect rather than always getting it.
- Sink registration returns `esp_err_t` instead of `0`/`-1`, and reports
  `ESP_ERR_NOT_FOUND` when unregistering something that was never registered.
- `MAX_SINKS` and `FORMAT_BUF_SIZE` become Kconfig options.
