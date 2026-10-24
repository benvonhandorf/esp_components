#include "tone_gen.h"

#include <math.h>
#include <string.h>

#define DEFAULT_UPDATE_MS 5

/* Sweep frequency at sample `pos` of `duration`. */
static double sweep_hz(const tone_gen_t *g)
{
    const tone_gen_config_t *c = &g->cfg;
    double t = (double)g->pos / (double)c->duration_samples;
    if (t > 1.0) {
        t = 1.0;
    }
    /* A log sweep spends equal time per octave, which is how audio hardware is
     * actually judged. */
    if (c->logarithmic) {
        return c->start_hz * pow(c->end_hz / c->start_hz, t);
    }
    return c->start_hz + (c->end_hz - c->start_hz) * t;
}

esp_err_t tone_gen_init(tone_gen_t *g, const tone_gen_config_t *cfg)
{
    if (!g || !cfg || cfg->sample_rate_hz == 0 || cfg->peak < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    double nyquist = cfg->sample_rate_hz / 2.0;
    if (!(cfg->start_hz > 0.0) || !(cfg->end_hz > 0.0) ||
        cfg->start_hz >= nyquist || cfg->end_hz >= nyquist) {
        return ESP_ERR_INVALID_ARG;
    }
    bool sweeping = cfg->end_hz != cfg->start_hz;
    if (sweeping && cfg->duration_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(g, 0, sizeof(*g));
    g->cfg = *cfg;
    g->sweeping = sweeping;

    uint64_t ramp = (uint64_t)cfg->ramp_ms * cfg->sample_rate_hz / 1000;
    /* The two edges of a finite signal must not overlap. */
    if (cfg->duration_samples && ramp * 2 > cfg->duration_samples) {
        ramp = cfg->duration_samples / 2;
    }
    g->ramp_samples = (uint32_t)ramp;

    uint64_t update = (uint64_t)(cfg->update_ms ? cfg->update_ms : DEFAULT_UPDATE_MS) *
                      cfg->sample_rate_hz / 1000;
    g->update_samples = update ? (uint32_t)update : 1;
    g->update_left = 0;   /* a sweep computes its first step on the first sample */

    g->step = tone_gen_phase_step(cfg->start_hz, cfg->sample_rate_hz);
    return ESP_OK;
}

void tone_gen_release(tone_gen_t *g)
{
    if (!g || g->releasing || g->done) {
        return;
    }
    /* Already inside the closing ramp of a finite signal: it is on its way out. */
    if (g->cfg.duration_samples && g->pos + g->ramp_samples >= g->cfg.duration_samples) {
        return;
    }
    g->releasing = true;
    g->release_pos = 0;
    if (g->ramp_samples == 0) {
        g->done = true;
    }
}

size_t tone_gen_fill(tone_gen_t *g, int16_t *out, size_t max)
{
    if (!g || !out || g->done) {
        return 0;
    }

    const uint64_t duration = g->cfg.duration_samples;
    const uint32_t ramp = g->ramp_samples;
    const int32_t peak = g->cfg.peak;
    size_t n = 0;

    while (n < max) {
        if (g->sweeping) {
            if (g->update_left == 0) {
                g->step = tone_gen_phase_step(sweep_hz(g), g->cfg.sample_rate_hz);
                g->update_left = g->update_samples;
            }
            g->update_left--;
        }

        int32_t sample = (tone_gen_sine(g->phase) * peak) >> 15;

        /* Envelope: the lowest of the opening ramp, the closing ramp and the
         * release ramp. Nearly every sample is mid-signal and skips all of it. */
        int32_t gain = 32768;
        if (g->pos < ramp) {
            gain = tone_gen_ramp_gain((uint32_t)g->pos, ramp);
        }
        if (duration && g->pos + ramp >= duration) {
            int32_t closing = tone_gen_ramp_gain((uint32_t)(duration - g->pos), ramp);
            if (closing < gain) {
                gain = closing;
            }
        }
        if (g->releasing) {
            int32_t fade = tone_gen_ramp_gain(ramp - g->release_pos, ramp);
            if (fade < gain) {
                gain = fade;
            }
        }
        if (gain != 32768) {
            sample = (sample * gain) >> 15;
        }

        out[n++] = (int16_t)sample;
        g->phase += g->step;
        g->pos++;
        if (g->releasing) {
            g->release_pos++;
        }

        if ((duration && g->pos >= duration) ||
            (g->releasing && g->release_pos >= ramp)) {
            g->done = true;
            break;
        }
    }
    return n;
}

bool tone_gen_done(const tone_gen_t *g)
{
    return !g || g->done;
}

double tone_gen_current_hz(const tone_gen_t *g)
{
    return (double)g->step * (double)g->cfg.sample_rate_hz / 4294967296.0;
}
