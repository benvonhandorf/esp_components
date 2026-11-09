# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-11-09

First release.

- UAC2 microphone-only TinyUSB device with run-time descriptors: channel count 1-16,
  any sample rate, 16- or 24-bit, one isochronous asynchronous IN endpoint.
- `usb_mic_check()` and friends: the per-packet budget of the controller (S2/S3 full
  speed: 712 bytes per 1 ms packet from the 256-word FIFO; P4/S31 high speed), and
  which rates and channel counts fit. Host-tested.
- Verified on an ESP32-S3 against Linux (`snd-usb-audio`): 2 channels at 48 kHz, 60 s
  with no gaps. High-speed targets are untested.
