# pdm_decimator

Software PDM-to-PCM decimation for a raw PDM receiver, one to eight microphones.

Most ESP32 chips have one hardware PDM-to-PCM filter. It handles two channels and
divides by a fixed 64, so a microphone clocked at 4.8 MHz comes out at 75 kHz PCM,
which is not enough bandwidth for an ultrasonic part, and a board with more than
two microphones cannot use it at all. Both cases take the raw one-bit stream from
the I2S receiver and filter it in software. This is that filter.

It touches no hardware. It turns words of bits into 16-bit PCM; getting the words
out of the I2S peripheral stays with the application.

```c
#include "pdm_decimator.h"

pdm_decimator_config_t cfg = {
    .filter     = PDM_DECIMATOR_TWO_STAGE,
    .channels   = 4,
    .allow_simd = true,
};
pdm_decimator_t *dec;
ESP_ERROR_CHECK(pdm_decimator_create(&cfg, &dec));
ESP_ERROR_CHECK(pdm_decimator_reserve(dec, 1024));   /* no allocation on the receive path */

size_t produced;
ESP_ERROR_CHECK(pdm_decimator_process(dec, raw, frames, pcm, &produced));
```

## Input and output

One input **frame** is `channels` consecutive `uint16_t` words, one per
microphone, each holding the next 16 bits of that microphone's stream. The output
is `channels` interleaved `int16_t` per frame, channel *c* in is channel *c* out,
full scale +-32767 for a stream of all ones or all zeros.

Which word belongs to which microphone is the application's wiring. Two data pins
with a left and a right microphone each give four channels in whatever order the
receiver delivers its slots; this library does not know and does not reorder.
`lsb_first` says which end of a word is the oldest sample.

## Two filters

| | `PDM_DECIMATOR_SINC6` | `PDM_DECIMATOR_TWO_STAGE` |
|---|---|---|
| Structure | sixth-order sinc (CIC), table lookups | sinc^4 by 8 over bytes, then 24-tap FIR by 2 |
| Ratio | 16, 32, 48 or 64 | 16 (4.8 MHz to 300 kHz) |
| Response at 80 kHz, 4.8 MHz clock | about -6 dB | about -3 dB |
| Cost, 2 ch, 240 MHz S3 | about 300 cycles per output frame | about 160 with the vector unit |
| Vector code | none | stage 2, on the ESP32-S3 |

They are different filters: the two-stage reads up to about 3 dB higher than the
sinc^6 towards 80 kHz because it droops less. Compare microphones against each
other with the same filter, not across filters.

**Cost scales with channels.** The two-stage filter is about 80 cycles per channel
per output frame at 300 kHz, so at 240 MHz four channels is roughly 40% of one core
for the filter alone and six or more should be split across both cores. That
scheduling is the application's.

## The portable C is deliberate

`pdm_stage2_scalar.c` is the second stage in plain C. It is the reference the
vector version in `pdm_stage2_simd.c` must match bit for bit, it is the only
implementation on a chip without that unit, and `allow_simd = false` selects it at
runtime so a result can be checked against it. The vector file is added by
`CMakeLists.txt` only for `esp32s3` and is the one file here that cannot be built or
run on a host. Change the arithmetic in the scalar file first.

## Build notes

- **Built `-O2`, PRIVATE to this component, whatever the project asks for.** The
  filter runs once per received word. At `-Og`, or at 160 MHz on an S3, a 300 kHz
  stereo stream overruns the receiver, and the only symptom is a noise floor 20 dB
  too high. Use 240 MHz on the S3.
- One handle is one stream, and calls on a handle must not overlap.
- The two-stage work buffer comes from internal RAM (the vector loads want it) and
  grows to the largest call seen; `pdm_decimator_reserve()` sizes it up front.

## Testing

```sh
make -C pdm_decimator/test
```

Plain `gcc`, no ESP-IDF. It feeds both filters a real second-order sigma-delta
bitstream and checks the level that comes out against each filter's response
worked out independently, plus: DC, the two filters against each other, `lsb_first`
against bit-reversed input, chunked against whole-stream output, four channels
carrying four tones with no crosstalk, ratios, and argument errors.

The vector stage 2 against the scalar one can only be compared on the chip. In the
`esp_board_bringup` rig, `audio pdmbench` does that and prints `match` or the
largest difference.
