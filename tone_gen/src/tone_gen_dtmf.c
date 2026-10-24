#include "tone_gen.h"

#include <ctype.h>
#include <string.h>

#define LOW_COUNT  4
#define HIGH_COUNT 4

static const uint16_t low_hz[LOW_COUNT]   = {697, 770, 852, 941};
static const uint16_t high_hz[HIGH_COUNT] = {1209, 1336, 1477, 1633};

/* Row-major, matching the keypad. */
static const char keys[LOW_COUNT][HIGH_COUNT] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

bool tone_gen_dtmf_freqs(char digit, uint16_t *low, uint16_t *high)
{
    char c = (char)toupper((unsigned char)digit);
    for (int r = 0; r < LOW_COUNT; r++) {
        for (int k = 0; k < HIGH_COUNT; k++) {
            if (keys[r][k] == c) {
                if (low) *low = low_hz[r];
                if (high) *high = high_hz[k];
                return true;
            }
        }
    }
    return false;
}

static bool is_separator(char c)
{
    return c == ' ' || c == '-';
}

static bool is_pause(char c)
{
    return c == ',';
}

esp_err_t tone_gen_dtmf_validate(const char *digits, size_t *bad_index)
{
    if (!digits) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t significant = 0;
    for (size_t i = 0; digits[i]; i++) {
        char c = digits[i];
        if (is_separator(c)) {
            continue;
        }
        if (!is_pause(c) && !tone_gen_dtmf_freqs(c, NULL, NULL)) {
            if (bad_index) {
                *bad_index = i;
            }
            return TONE_GEN_ERR_INVALID_DIGIT;
        }
        significant++;
    }
    return significant ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static uint32_t ms_to_samples(uint32_t ms, uint32_t rate)
{
    return (uint32_t)((uint64_t)ms * rate / 1000);
}

uint64_t tone_gen_dtmf_total_samples(const tone_gen_dtmf_config_t *cfg, const char *digits)
{
    if (!cfg || !digits) {
        return 0;
    }
    uint64_t tone = ms_to_samples(cfg->tone_ms, cfg->sample_rate_hz);
    uint64_t gap = ms_to_samples(cfg->gap_ms, cfg->sample_rate_hz);
    uint64_t pause = ms_to_samples(cfg->pause_ms, cfg->sample_rate_hz);

    /* Count first, so the last key can be told apart: a gap follows every key
     * that something follows, and nothing follows the last. */
    size_t count = 0;
    for (size_t i = 0; digits[i]; i++) {
        if (!is_separator(digits[i])) {
            count++;
        }
    }

    uint64_t total = 0;
    size_t seen = 0;
    for (size_t i = 0; digits[i]; i++) {
        if (is_separator(digits[i])) {
            continue;
        }
        seen++;
        if (is_pause(digits[i])) {
            total += pause;
        } else {
            total += tone;
            if (seen < count) {
                total += gap;
            }
        }
    }
    return total;
}

/* Is there another character to dial after `index`? */
static bool more_after(const tone_gen_dtmf_t *g, size_t index)
{
    for (size_t i = index; g->digits[i]; i++) {
        if (!is_separator(g->digits[i])) {
            return true;
        }
    }
    return false;
}

/*
 * Move on to the next segment: a key, a gap, or a pause. A gap is its own
 * segment, scheduled when a key ends and something still follows it, so it is
 * absent after the last key and a trailing ',' is the only silence at the end.
 */
static void next_segment(tone_gen_dtmf_t *g, bool after_tone)
{
    if (after_tone && more_after(g, g->index) && g->gap_samples) {
        g->in_tone = false;
        g->current = 0;
        g->seg_pos = 0;
        g->seg_len = g->gap_samples;
        return;
    }

    while (g->digits[g->index] && is_separator(g->digits[g->index])) {
        g->index++;
    }
    char c = g->digits[g->index];
    if (!c) {
        g->done = true;
        return;
    }
    g->index++;

    g->seg_pos = 0;
    if (is_pause(c)) {
        g->in_tone = false;
        g->current = 0;
        g->seg_len = g->pause_samples;
        if (g->seg_len == 0) {
            next_segment(g, false);
        }
        return;
    }

    uint16_t lo = 0, hi = 0;
    tone_gen_dtmf_freqs(c, &lo, &hi);
    g->in_tone = true;
    g->current = (char)toupper((unsigned char)c);
    g->seg_len = g->tone_samples;
    g->low_phase = 0;
    g->high_phase = 0;
    g->low_step = tone_gen_phase_step_int(lo, g->cfg.sample_rate_hz);
    g->high_step = tone_gen_phase_step_int(hi, g->cfg.sample_rate_hz);
}

esp_err_t tone_gen_dtmf_init(tone_gen_dtmf_t *g, const tone_gen_dtmf_config_t *cfg,
                             const char *digits)
{
    if (!g || !cfg || !digits || cfg->sample_rate_hz < 4000 || cfg->tone_ms == 0 ||
        cfg->low_peak < 0 || cfg->high_peak < 0 ||
        (int32_t)cfg->low_peak + cfg->high_peak > 32767) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = tone_gen_dtmf_validate(digits, NULL);
    if (err != ESP_OK) {
        return err;
    }

    memset(g, 0, sizeof(*g));
    g->cfg = *cfg;
    g->digits = digits;
    g->tone_samples = ms_to_samples(cfg->tone_ms, cfg->sample_rate_hz);
    g->gap_samples = ms_to_samples(cfg->gap_ms, cfg->sample_rate_hz);
    g->pause_samples = ms_to_samples(cfg->pause_ms, cfg->sample_rate_hz);

    uint32_t ramp = ms_to_samples(cfg->ramp_ms, cfg->sample_rate_hz);
    /* The two edges of a key must not overlap. */
    if (ramp * 2 > g->tone_samples) {
        ramp = g->tone_samples / 2;
    }
    g->ramp_samples = ramp;

    next_segment(g, false);
    return ESP_OK;
}

size_t tone_gen_dtmf_fill(tone_gen_dtmf_t *g, int16_t *out, size_t max)
{
    if (!g || !out || g->done) {
        return 0;
    }

    size_t n = 0;
    while (n < max && !g->done) {
        int32_t sample = 0;
        if (g->in_tone) {
            int32_t low = (tone_gen_sine(g->low_phase) * (int32_t)g->cfg.low_peak) >> 15;
            int32_t high = (tone_gen_sine(g->high_phase) * (int32_t)g->cfg.high_peak) >> 15;
            sample = low + high;

            /* Opening and closing edges of this key. */
            if (g->seg_pos < g->ramp_samples) {
                sample = (sample * tone_gen_ramp_gain(g->seg_pos, g->ramp_samples)) >> 15;
            }
            uint32_t remaining = g->seg_len - g->seg_pos;
            if (remaining <= g->ramp_samples) {
                int32_t gain = tone_gen_ramp_gain(remaining, g->ramp_samples);
                sample = (sample * gain) >> 15;
            }
            g->low_phase += g->low_step;
            g->high_phase += g->high_step;
        }
        out[n++] = (int16_t)sample;

        if (++g->seg_pos >= g->seg_len) {
            next_segment(g, g->in_tone);
        }
    }
    return n;
}

void tone_gen_dtmf_release(tone_gen_dtmf_t *g)
{
    if (!g || g->done) {
        return;
    }
    if (!g->in_tone) {
        g->done = true;
        return;
    }
    /* Cut the key short to its closing ramp, and dial nothing after it. */
    uint32_t remaining = g->seg_len - g->seg_pos;
    if (remaining > g->ramp_samples) {
        g->seg_len = g->seg_pos + g->ramp_samples;
    }
    g->digits = "";
    g->index = 0;
    g->gap_samples = 0;
}

bool tone_gen_dtmf_done(const tone_gen_dtmf_t *g)
{
    return !g || g->done;
}

char tone_gen_dtmf_current(const tone_gen_dtmf_t *g)
{
    return g ? g->current : 0;
}
