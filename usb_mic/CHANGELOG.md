# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.1] - 2026-11-09

### Fixed

- **Only the TinyUSB task touches TinyUSB's audio FIFO now.** `usb_mic_write()` used to
  write into it from the producer's task while TinyUSB cleared it from its own when the
  host closed the stream, which could corrupt the FIFO's indices. It writes into a stream
  buffer, and the TinyUSB task moves that across. On a four-channel board the chip hung
  around the end of a recording, and it no longer does over repeated cycles.
- Nobody-listening data is discarded by draining, not by resetting the buffer under a
  producer that may be sending.

## [0.1.0] - 2026-11-09

First release.

- UAC2 microphone-only TinyUSB device with run-time descriptors: channel count 1-16,
  any sample rate, 16- or 24-bit, one isochronous asynchronous IN endpoint.
- `usb_mic_check()` and friends: the per-packet budget of the controller (S2/S3 full
  speed: 712 bytes per 1 ms packet from the 256-word FIFO; P4/S31 high speed), and
  which rates and channel counts fit. Host-tested.
- Verified on an ESP32-S3 against Linux (`snd-usb-audio`): 2 channels at 48 kHz, 60 s
  with no gaps. High-speed targets are untested.
