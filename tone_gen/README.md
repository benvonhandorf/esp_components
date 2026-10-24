# tone_gen

Audio test-signal generation for ESP-IDF: steady tones and sweeps with raised-cosine
edges. Mono `int16_t` out, pull model, no heap, no hardware.

```c
tone_gen_config_t cfg = {
    .sample_rate_hz = 48000,
    .start_hz = 1000.0, .end_hz = 1000.0,
    .duration_samples = 48000,            /* 0 = continuous until tone_gen_release() */
    .peak = 8000,
    .ramp_ms = 5,
};
tone_gen_t gen;
tone_gen_init(&gen, &cfg);

int16_t block[240];
size_t n;
while ((n = tone_gen_fill(&gen, block, 240)) > 0) {
    write_to_i2s(block, n);              /* you choose slots, width and channel */
}
```

## Why a table, and why this one

Three ways to make a sine at audio rates, judged on size rather than speed:

| | Data | Cost |
|---|---|---|
| Per-sample `sinf()` | none | softfloat on an FPU-less ESP32-C3 takes a large part of a core |
| Recursive oscillator | none | a `cos()` per frequency change, and fixed-point amplitude drift |
| **Quarter-wave table, interpolated** | **258 bytes of flash** | integer only, the same speed on every chip |

A 129-entry first quadrant is mirrored into the other three and linearly interpolated with
16 fractional bits. Worst error is about 1 LSB of 32767; harmonics of a full-scale tone sit
below -85 dBc, under what any amplifier under test adds. A 32-bit phase accumulator gives
sub-millihertz frequency resolution and means no frequency has to divide into the buffer.

## Block-size independence

`tone_gen_fill()` produces the same waveform whatever block size it is called with (tested
at 1, 7, 240 and 512), including across a sweep's frequency updates. A DMA loop, a file
writer and a test all hear one signal.

## Stopping without a click

`tone_gen_release()` starts a raised-cosine fade-out from wherever the signal is. `fill()`
carries on through the ramp and then returns 0. Cutting a tone mid-cycle thumps a speaker.

## Tests

```sh
make -C tone_gen/test              # WAV=1 also writes tone.wav and sweep_log.wav
```
