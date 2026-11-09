#pragma once

/*
 * Software PDM-to-PCM decimation for a raw PDM receiver, any number of
 * microphones.
 *
 * The ESP32-S3 has a hardware PDM-to-PCM filter with a fixed divide-by-64; it handles
 * several data lines (four channels from two lines measured on hardware), but its output
 * rate is the clock over 64. A microphone that needs another ratio (an ultrasonic MEMS
 * part at 4.8 MHz wanting 300 kHz, say) has to take the raw one-bit stream instead and
 * filter it in software, and that raw receiver captures a single line. This is
 * that filter. It touches no hardware: it turns words of bits into 16-bit PCM,
 * and getting the words out of the I2S peripheral stays with the caller.
 *
 * INPUT. One "frame" is `channels` consecutive uint16_t words, one word per
 * microphone, each holding the next 16 bits of that microphone's stream. Which
 * word belongs to which microphone is the caller's wiring, not this library's:
 * two data lines with a left and a right microphone each give four channels in
 * whatever order the receiver delivers its slots. Channel c of the output is
 * channel c of the input.
 *
 * OUTPUT. `channels` interleaved int16_t per output frame, full scale +-32767
 * for a stream that is all ones or all zeros.
 *
 * TWO FILTERS, because the trade is real:
 *
 *   PDM_DECIMATOR_SINC6      a sixth-order sinc (CIC) by `ratio`, 16 to 64 in
 *                            steps of 16. Plain C everywhere. Droops about 6 dB
 *                            at 80 kHz for ratio 16 at a 4.8 MHz clock. The
 *                            ratio is the only knob that picks the output rate.
 *   PDM_DECIMATOR_TWO_STAGE  a sinc^4 by 8 over bytes, then a 24-tap FIR by 2.
 *                            Always ratio 16 (4.8 MHz to 300 kHz). Roughly half
 *                            the cost of SINC6 where the FIR runs on the
 *                            ESP32-S3's vector unit, and flatter in the
 *                            passband. It is a different filter, not a faster
 *                            way to compute the same one.
 *
 * The vector code is chosen at runtime (`allow_simd`) and a plain C version of
 * the same arithmetic is always built, so a result can be checked against it
 * and a chip without the vector unit still works.
 *
 * THREADING. A decimator is state: it carries filter history between calls, so
 * one handle belongs to one stream and calls on it must not overlap. Separate
 * handles are independent.
 *
 * CPU. The per-word cost is why this is built -O2 whatever the project's
 * optimisation level: at -Og a 300 kHz stereo stream overruns the receiver, and
 * the symptom is a noise floor that is too high, with no error anywhere.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDM_DECIMATOR_ERR_BASE      0x3F000
/* The ratio is not one this filter supports. */
#define PDM_DECIMATOR_ERR_BAD_RATIO (PDM_DECIMATOR_ERR_BASE + 1)
/* channels is outside 1..PDM_DECIMATOR_MAX_CHANNELS. */
#define PDM_DECIMATOR_ERR_BAD_CHANNELS (PDM_DECIMATOR_ERR_BASE + 2)

#define PDM_DECIMATOR_MAX_CHANNELS  8

typedef enum {
    PDM_DECIMATOR_SINC6 = 0,
    PDM_DECIMATOR_TWO_STAGE,
} pdm_decimator_filter_t;

typedef struct {
    pdm_decimator_filter_t filter;
    int channels;           /* 1..PDM_DECIMATOR_MAX_CHANNELS */
    /* Raw bits in per output sample. SINC6: 16, 32, 48 or 64. TWO_STAGE: 16,
     * or 0 for the same thing. */
    int ratio;
    /* Which end of each 16-bit word is the oldest sample. False: the most
     * significant bit is oldest. True: the least significant bit is. */
    bool lsb_first;
    /* Use the vector unit if this chip has one. False forces the portable C,
     * which is what a result is checked against. */
    bool allow_simd;
} pdm_decimator_config_t;

typedef struct pdm_decimator pdm_decimator_t;

/* ESP_ERR_INVALID_ARG for a NULL pointer, PDM_DECIMATOR_ERR_BAD_CHANNELS and
 * PDM_DECIMATOR_ERR_BAD_RATIO for those fields, ESP_ERR_NO_MEM when the tables
 * or history do not fit. *out is NULL on any error. */
esp_err_t pdm_decimator_create(const pdm_decimator_config_t *cfg, pdm_decimator_t **out);

/* NULL is accepted. */
void pdm_decimator_destroy(pdm_decimator_t *d);

/*
 * Decimate `frames` raw frames into `out`, which must hold at least
 * pdm_decimator_max_output(d, frames) frames. *produced is the number of output
 * frames written; it is not `frames / ratio` on any one call, because the
 * filter carries a phase between calls -- feeding a stream in chunks of any
 * size gives exactly the output of feeding it whole.
 *
 * TWO_STAGE keeps a work buffer sized to the largest call so far and grows it
 * the first time a larger one arrives, which allocates. Call
 * pdm_decimator_reserve() with the largest chunk first if that must not happen
 * on the receive path. ESP_ERR_NO_MEM is the only runtime failure.
 */
esp_err_t pdm_decimator_process(pdm_decimator_t *d, const uint16_t *raw, size_t frames,
                                int16_t *out, size_t *produced);

/* Size the work buffer for calls of up to max_frames so that
 * pdm_decimator_process() never allocates. A no-op for SINC6. */
esp_err_t pdm_decimator_reserve(pdm_decimator_t *d, size_t max_frames);

/* The most output frames one call with `frames` input frames can produce. */
size_t pdm_decimator_max_output(const pdm_decimator_t *d, size_t frames);

/* Raw frames that yield `out_frames` output frames: what to read from the
 * receiver to fill an output buffer. */
size_t pdm_decimator_input_frames(const pdm_decimator_t *d, size_t out_frames);

int pdm_decimator_channels(const pdm_decimator_t *d);

/* Raw bits per output sample, whatever the config said (so 16 for a TWO_STAGE
 * asked for 0). The output rate is the bit clock divided by this. */
int pdm_decimator_ratio(const pdm_decimator_t *d);

pdm_decimator_filter_t pdm_decimator_filter(const pdm_decimator_t *d);

/* Whether the vector unit is in use: the chip has one, the filter has code for
 * it, and allow_simd was set. */
bool pdm_decimator_uses_simd(const pdm_decimator_t *d);

#ifdef __cplusplus
}
#endif
