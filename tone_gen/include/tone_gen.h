#ifndef TONE_GEN_H
#define TONE_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Audio test-signal generation: a sine oscillator, steady tones and sweeps with
 * raised-cosine edges. Output is mono int16 and nothing else -- slot layout,
 * word width and channel placement belong to whoever writes it to a bus, which
 * is what lets the same generator feed any I2S codec, a file or a host test.
 *
 * Pull model: the caller asks for as many samples as it wants and gets exactly
 * the same stream whatever the block size, so a DMA loop, a file writer and a
 * unit test all see one waveform. Nothing here allocates, and nothing touches
 * hardware.
 *
 * Integer only per sample, no libm. An ESP32-C3 has no FPU, and a softfloat
 * sinf() at audio rates would cost a large part of a core -- exactly when the
 * point of a test signal is to find out whether the *hardware* keeps up. The
 * one floating-point step is the sweep's frequency update, once per
 * update_ms (5 ms by default), not once per sample.
 */

#define ESP_ERR_TONE_GEN_BASE 0x41000

/*
 * The oscillator: a full-cycle 32-bit phase accumulator over a 129-entry
 * quarter-wave table in flash (258 bytes, no RAM, no init), mirrored into the
 * other three quadrants and linearly interpolated with 16 fractional bits.
 * Worst error against sin() is about 1 LSB of 32767, which is below the noise
 * of any amplifier this is meant to measure.
 *
 * 32 bits of phase means a frequency is resolved to a fraction of a millihertz
 * at audio rates and no frequency has to divide into the buffer length.
 */

/* Q15 sine: phase 0 is 0, phase 0x40000000 is +32767, phase 0x80000000 is 0. */
int16_t  tone_gen_sine(uint32_t phase);

/* Phase increment per sample for `hz` at `rate_hz`. The double version rounds
 * to nearest; the integer version needs no floating point at all. */
uint32_t tone_gen_phase_step(double hz, uint32_t rate_hz);
uint32_t tone_gen_phase_step_int(uint32_t hz, uint32_t rate_hz);

/* Raised-cosine edge, sin^2(pi*n/(2*span)), as Q15 gain. Position n of span;
 * 32768 (unity) when span is 0 or n >= span. */
int32_t  tone_gen_ramp_gain(uint32_t n, uint32_t span);

/* ------------------------------------------------------------------ */
/* Steady tone and sweep                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t sample_rate_hz;
    double   start_hz;
    double   end_hz;            /* equal to start_hz for a steady tone */
    bool     logarithmic;       /* sweeps only: equal time per octave */
    /* Length in samples. 0 means continuous, which runs until
     * tone_gen_release() and cannot sweep (a sweep needs a duration). */
    uint64_t duration_samples;
    int16_t  peak;              /* amplitude in counts, 0..32767 */
    /* Edge length. Applied at both ends of a finite signal and on release,
     * clamped to half the duration. 0 gives hard edges. */
    uint16_t ramp_ms;
    /* How often a sweep recomputes its frequency; 0 means 5. Smaller is
     * smoother and costs a pow() per update on a log sweep. */
    uint16_t update_ms;
} tone_gen_config_t;

/*
 * The generator's state. The fields are public only so a caller can allocate
 * one without heap; treat them as private.
 */
typedef struct {
    tone_gen_config_t cfg;
    uint32_t phase;
    uint32_t step;
    uint32_t ramp_samples;
    uint32_t update_samples;
    uint32_t update_left;       /* samples until the next sweep frequency update */
    bool sweeping;
    uint64_t pos;               /* samples produced so far */
    uint32_t release_pos;       /* samples produced since release */
    bool releasing;
    bool done;
} tone_gen_t;

/* ESP_ERR_INVALID_ARG for a frequency at or above Nyquist or not above zero, a
 * negative peak, a zero rate, or a sweep with no duration. */
esp_err_t tone_gen_init(tone_gen_t *g, const tone_gen_config_t *cfg);

/*
 * Write up to `max` samples and return how many were written. Fewer than `max`
 * means the signal ended inside this call; 0 means it already had. A caller
 * that wants continuous output keeps calling until it gets 0.
 */
size_t tone_gen_fill(tone_gen_t *g, int16_t *out, size_t max);

/*
 * Start the fade-out now, from wherever the signal is. fill() keeps producing
 * samples through the ramp and then ends. Phase is never cut mid-cycle, which
 * is what would thump a speaker. A signal already in its closing ramp is left
 * to finish.
 */
void tone_gen_release(tone_gen_t *g);

bool   tone_gen_done(const tone_gen_t *g);
double tone_gen_current_hz(const tone_gen_t *g);

/* ------------------------------------------------------------------ */
/* DTMF dialling                                                       */
/* ------------------------------------------------------------------ */

/* A character in a dial string that is not 0-9 * # A-D, a comma, or a separator. */
#define TONE_GEN_ERR_INVALID_DIGIT (ESP_ERR_TONE_GEN_BASE + 1)

/*
 * Each key is two sine tones at once, one from a low group (the row) and one
 * from a high group (the column), so no single frequency or harmonic of one can
 * be mistaken for a key:
 *
 *           1209  1336  1477  1633 Hz
 *     697     1     2     3     A
 *     770     4     5     6     B
 *     852     7     8     9     C
 *     941     *     0     #     D
 *
 * A dial string is those keys, plus ',' for a pause of pause_ms of silence. A
 * space or '-' is skipped, so "555-1234" dials as "5551234". Letters are
 * case-insensitive.
 */
typedef struct {
    uint32_t sample_rate_hz;   /* at least 4000: Nyquist must clear 1633 Hz */
    uint16_t tone_ms;          /* key down; the ITU minimum is 40, 100 is safe */
    uint16_t gap_ms;           /* between keys; same minimum */
    uint16_t pause_ms;         /* what ',' adds */
    uint16_t ramp_ms;          /* raised-cosine edge on every key, 0 for hard edges */
    /*
     * Peak of each tone in counts. The high tone is conventionally 1-3 dB
     * louder than the low one ("twist") because lines lose more at high
     * frequency. low_peak + high_peak must not exceed 32767, which is what
     * guarantees the sum of the two never clips.
     */
    int16_t low_peak;
    int16_t high_peak;
} tone_gen_dtmf_config_t;

/* 100 ms keys, 100 ms gaps, 2 s pauses, 3 ms edges, low tone at about -8 dBFS
 * and the high tone 2 dB above it (-6 dBFS). */
#define TONE_GEN_DTMF_CONFIG_DEFAULT(rate)                                   \
    {                                                                         \
        .sample_rate_hz = (rate), .tone_ms = 100, .gap_ms = 100,              \
        .pause_ms = 2000, .ramp_ms = 3, .low_peak = 13000, .high_peak = 16367, \
    }

typedef struct {
    tone_gen_dtmf_config_t cfg;
    const char *digits;
    size_t index;               /* the next character to look at */
    uint32_t tone_samples, gap_samples, pause_samples, ramp_samples;
    uint32_t seg_pos, seg_len;  /* position inside, and length of, the current segment */
    uint32_t low_phase, high_phase, low_step, high_step;
    char current;               /* the key being played; 0 in a gap or pause */
    bool in_tone;
    bool done;
} tone_gen_dtmf_t;

/* The two frequencies for `digit`, or false if it is not a key. */
bool tone_gen_dtmf_freqs(char digit, uint16_t *low_hz, uint16_t *high_hz);

/*
 * Check a dial string before playing it, so a typo is reported rather than
 * heard. On TONE_GEN_ERR_INVALID_DIGIT, *bad_index (if not NULL) is the
 * offending character. ESP_ERR_INVALID_ARG for a string with nothing to dial.
 */
esp_err_t tone_gen_dtmf_validate(const char *digits, size_t *bad_index);

/* `digits` must outlive the generator. Validates it, and the configuration. */
esp_err_t tone_gen_dtmf_init(tone_gen_dtmf_t *g, const tone_gen_dtmf_config_t *cfg,
                             const char *digits);

/* As tone_gen_fill(): the same samples whatever the block size; 0 once finished. */
size_t tone_gen_dtmf_fill(tone_gen_dtmf_t *g, int16_t *out, size_t max);

/* Stop dialling. A key that is sounding finishes its closing ramp; silence ends
 * at once. */
void tone_gen_dtmf_release(tone_gen_dtmf_t *g);

bool tone_gen_dtmf_done(const tone_gen_dtmf_t *g);

/* The key sounding now, or 0 in a gap or pause. For progress display. */
char tone_gen_dtmf_current(const tone_gen_dtmf_t *g);

/* Samples the whole string produces, for reporting a duration before playing. */
uint64_t tone_gen_dtmf_total_samples(const tone_gen_dtmf_config_t *cfg, const char *digits);

#ifdef __cplusplus
}
#endif

#endif /* TONE_GEN_H */
