# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.1] - 2026-11-10

### Changed

- Takes `usb_mic` 0.2.0, which moves to `espressif/tinyusb` ^0.21.0~4. No code change here.

## [0.1.0] - 2026-11-10

First release, extracted from the `audio-usb` command of `esp_board_bringup`.

- `audio_stream_usb_check()`, `_start()`, `_stop()`, `_active()`: a `usb_mic` device fed by
  an `audio_stream` stream, with the `before_usb` hook for the application's own preparation
  (a console warning, silencing the log, a reset timer).
