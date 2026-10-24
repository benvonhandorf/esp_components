# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-10-24

Extracted from `esp_board_bringup`'s `main/audio/tone.c`.

### Changed from the original

- **A 129-entry quarter-wave table in flash replaces a 1024-entry full-cycle table in
  RAM.** 258 bytes instead of 2 KB, no runtime init. Interpolation is 16 fractional bits
  rather than 8: with half the points, 8 bits would have cost about 3 LSB.
- **The generator is a pull model with its own state**, so a block of any size gives the
  same samples. Sweep frequency updates are on a sample count (`update_ms`) rather than
  on the caller's block boundary.
- **Fade-out is part of the generator** (`tone_gen_release()`), not a separate block the
  caller writes after stopping.
- Output is mono `int16_t`; slot layout and word width belong to the caller.
