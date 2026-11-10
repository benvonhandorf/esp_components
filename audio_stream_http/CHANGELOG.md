# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-11-10

First release.

- `audio_stream_http_serve()`: an endless WAV response from an `esp_http_server` handler,
  through an asynchronous request copy, so the server's task is never held.
- `audio_stream_http_parse_query()` for `?rate=&bits=&ch=`, `audio_stream_http_stop()`,
  `audio_stream_http_get_stats()`.
- One listener at a time; a listener that goes away frees the microphone and closes its
  socket.
