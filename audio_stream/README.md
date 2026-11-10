# audio_stream

Stream a microphone somewhere else. It resolves a request (rate, channels, 16- or 24-bit)
against what the microphone offers, decimates a raw PDM source, converts to interleaved
little-endian PCM, and hands it to a **sink**. The sinks are separate components, so a
project takes only the one it uses:

| Component | Sink |
|---|---|
| `audio_stream_usb` | a USB Audio Class 2 microphone, over `usb_mic` |
| `audio_stream_http` | an endless WAV response from an `esp_http_server` handler |
| your own | an `audio_stream_sink_t`: three callbacks |

This component never opens, closes or reconfigures a microphone and knows nothing about
I2S: the project describes what it has.

```c
static esp_err_t read_mic(void *ctx, void *buf, size_t len, size_t *got, uint32_t timeout_ms)
{
    return my_i2s_read(buf, len, got, timeout_ms);
}

audio_stream_source_t src = {
    .rate_hz = 48000, .channels = 2, .slot_bytes = 4, .data_bits = 24,
    .read = read_mic,
};

audio_stream_want_t want = {.nch = 1, .chan = {1}, .bits = 16};   // the right microphone, 16-bit
audio_stream_fmt_t fmt;
audio_stream_why_t why;
if (audio_stream_resolve(&src, &want, &fmt, &why) != ESP_OK) {
    // why.code says which request could not be met; why.src_rate etc. say what would be
}

audio_stream_start(&src, &fmt, &my_sink, my_ctx, &(audio_stream_options_t){.owner = "mine"});
```

## What it does for you

- **Resolves a request, and says why not.** A rate the microphone is not running at is
  refused, with the rate it runs at in `why`; it does not resample. A raw PDM source has no
  rate until a decimation ratio is chosen, so there the rate is the bit clock over 64, 48,
  32 or 16 and `why.raw_rates` lists them. Every refusal is a code and some numbers: the
  application words them, so this component has no prose to translate.
- **Selects and converts.** Any subset of channels, in any order, from 16- or 32-bit slots
  to 16- or 24-bit output. Host-tested.
- **Decimates a raw PDM source** with `pdm_decimator` (portable C, the S3 vector stage when
  there is one), and counts the cycles so a caller can report the load.
- **Runs one stream at a time.** Two readers of one receiver would each get alternate
  blocks, so a second start returns `AUDIO_STREAM_ERR_BUSY` and `audio_stream_owner()` names
  the first. Start and stop may come from different tasks.
- **Writes a streaming WAV header** (`audio_stream_wav_header()`): plain PCM for mono or
  stereo 16-bit, `WAVE_FORMAT_EXTENSIBLE` otherwise, with sizes of `0xFFFFFFFF` that ffmpeg,
  sox and VLC read as "until the end".

## The queue

`audio_stream_queue.h` is for a sink that can block, such as a socket. The stream task
writes a block and returns at once; a drain task empties the buffer into a blocking
`send()`. When the buffer is full the whole block is dropped and counted, so a WiFi stall
costs a gap in the recording and never an overrun in the receiver.

With external RAM the buffer goes there (`use_psram`): a second of 4-channel 48 kHz audio
is 384 kB. Without it the buffer is internal and capped by the largest free block less a
reserve, so asking for too much shrinks the buffer rather than starving lwIP. The size
asked for is the caller's trade: a larger buffer rides out longer stalls, and turns a stall
into lasting delay for a live listener.

The ring is a lock-free single-producer, single-consumer byte ring with a host test that
runs a real producer and consumer thread against each other.

## Writing a sink

```c
static bool my_write(void *ctx, const uint8_t *pcm, size_t len)
{
    return send_somewhere(ctx, pcm, len);     // must not block for long; false ends the stream
}

static const audio_stream_sink_t my_sink = {
    .begin = NULL,               // optional: bring the sink up; failing here fails the start
    .write = my_write,
    .end = NULL,                 // optional: called once if begin() succeeded
    .release_on_end = true,      // a stream that ends by itself frees its place at once
};
```

`end()` and `write()` run in the stream task: do not call `audio_stream_stop()` from them.

## Testing

```sh
make -C test
```

Plain gcc, no ESP-IDF. The runner and the queue need FreeRTOS and are checked on the chip.
