# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-11-10

First release, extracted from the `audio-usb` command of `esp_board_bringup`.

- `audio_stream_resolve()`: a request (rate, channel list, 16/24-bit) against a source, with
  the facts of a refusal in `audio_stream_why_t`. Raw PDM sources pick a rate as the bit clock
  over a ratio (64, 48, 32, 16).
- `audio_stream_convert()`, `audio_stream_parse_channels()`, `audio_stream_wav_header()`
  (streaming WAV, plain PCM or WAVE_FORMAT_EXTENSIBLE). Host-tested.
- The runner: `audio_stream_start()` / `_stop()` / `_get_stats()`, one stream at a time, a
  sink interface (`begin`, `write`, `end`), decimation of a raw PDM source.
- `audio_stream_queue_*`: a lock-free single-producer ring in external RAM when the chip has
  it, drained by a task into a blocking `send()`; drops whole blocks and counts them.
