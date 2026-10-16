/*
 * The public face of the component: argument checking, and handing each call to
 * whichever filter the handle was made with. The filters themselves are in
 * pdm_sinc6.c and pdm_twostage.c.
 */
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "pdm_decimator.h"
#include "pdm_decimator_priv.h"

struct pdm_decimator {
    pdm_decimator_filter_t filter;
    int channels;
    int ratio;
    union {
        pdm_sinc6_t *sinc6;
        pdm_twostage_t *twostage;
    } f;
};

void *pdm_alloc_aligned16(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return aligned_alloc(16, (bytes + 15) & ~(size_t)15);
#endif
}

esp_err_t pdm_decimator_create(const pdm_decimator_config_t *cfg, pdm_decimator_t **out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->channels < 1 || cfg->channels > PDM_DECIMATOR_MAX_CHANNELS) {
        return PDM_DECIMATOR_ERR_BAD_CHANNELS;
    }

    int ratio = cfg->ratio;
    switch (cfg->filter) {
    case PDM_DECIMATOR_SINC6:
        if (ratio < 16 || ratio > 64 || (ratio % 16) != 0) {
            return PDM_DECIMATOR_ERR_BAD_RATIO;
        }
        break;
    case PDM_DECIMATOR_TWO_STAGE:
        if (ratio == 0) {
            ratio = 16;
        }
        if (ratio != 16) {
            return PDM_DECIMATOR_ERR_BAD_RATIO;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    pdm_decimator_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    d->filter = cfg->filter;
    d->channels = cfg->channels;
    d->ratio = ratio;

    esp_err_t err = cfg->filter == PDM_DECIMATOR_SINC6
        ? pdm_sinc6_create(cfg->channels, ratio, cfg->lsb_first, &d->f.sinc6)
        : pdm_twostage_create(cfg->channels, cfg->lsb_first, cfg->allow_simd, &d->f.twostage);
    if (err != ESP_OK) {
        free(d);
        return err;
    }
    *out = d;
    return ESP_OK;
}

void pdm_decimator_destroy(pdm_decimator_t *d)
{
    if (!d) {
        return;
    }
    if (d->filter == PDM_DECIMATOR_SINC6) {
        pdm_sinc6_destroy(d->f.sinc6);
    } else {
        pdm_twostage_destroy(d->f.twostage);
    }
    free(d);
}

esp_err_t pdm_decimator_process(pdm_decimator_t *d, const uint16_t *raw, size_t frames,
                                int16_t *out, size_t *produced)
{
    if (!d || !produced || (frames && (!raw || !out))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (d->filter == PDM_DECIMATOR_SINC6) {
        *produced = pdm_sinc6_process(d->f.sinc6, raw, frames, out);
        return ESP_OK;
    }
    return pdm_twostage_process(d->f.twostage, raw, frames, out, produced);
}

esp_err_t pdm_decimator_reserve(pdm_decimator_t *d, size_t max_frames)
{
    if (!d) {
        return ESP_ERR_INVALID_ARG;
    }
    return d->filter == PDM_DECIMATOR_TWO_STAGE ? pdm_twostage_reserve(d->f.twostage, max_frames)
                                                : ESP_OK;
}

size_t pdm_decimator_max_output(const pdm_decimator_t *d, size_t frames)
{
    /* One output per ratio/16 words, with a phase carried between calls, so a
     * call can produce one more than the plain quotient. The two-stage filter
     * makes exactly one output per word. */
    return frames / (size_t)(d->ratio / 16) + 1;
}

size_t pdm_decimator_input_frames(const pdm_decimator_t *d, size_t out_frames)
{
    return out_frames * (size_t)(d->ratio / 16);
}

int pdm_decimator_channels(const pdm_decimator_t *d)
{
    return d->channels;
}

int pdm_decimator_ratio(const pdm_decimator_t *d)
{
    return d->ratio;
}

pdm_decimator_filter_t pdm_decimator_filter(const pdm_decimator_t *d)
{
    return d->filter;
}

bool pdm_decimator_uses_simd(const pdm_decimator_t *d)
{
    return d->filter == PDM_DECIMATOR_TWO_STAGE && pdm_twostage_uses_simd(d->f.twostage);
}
