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

### Not yet verified on hardware

The register sequences follow Espressif's published driver. The chip ID values
(0x83, 0x11) and the volume scale are from the datasheet and have not yet been read
back from a part.
