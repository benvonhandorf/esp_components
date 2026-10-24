# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-10-24

Initial release, written for the M5Stack Cardputer Adv, which wires the part with no
MCLK pin.

- Slave-mode bring-up with `es8311_start()`, reporting which stage failed.
- Clock from the MCLK pin **or from the bit clock**. The pre-multiplier brings a 256 kHz
  bit clock (16-bit stereo at 8 kHz) to 2.048 MHz; Espressif's reference table has no
  row for that case.
- Clock and volume arithmetic in `es8311_calc.c`, host-tested against the rows of
  Espressif's reference table that use the same 256 x fs ratio.
- DAC volume in the part's own half-dB codes; 0 dB is the ceiling, since the codes above
  it are gain.
- Starts muted at the lowest volume code.

### Verified on hardware

On an M5Stack Cardputer Adv (ESP32-S3, no MCLK pin): the chip ID reads 0x83, 0x11,
version 0x01; the clock plan is /1 x8 at both 8 kHz and 48 kHz; playback and
microphone capture work, with a tone at 1 kHz heard by the microphone 50 to 57 dB above
the quiet floor.

### Not yet verified

The volume scale (0x00 = -95.5 dB, 0xBF = 0 dB) is from the datasheet and has been
heard to change the level but not measured against a calibrated source. The PDM
microphone path (`es8311_mic_t.digital`) has not been exercised.
