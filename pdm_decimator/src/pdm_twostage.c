/*
 * Two-stage decimation of a raw PDM stream: the same 4.8 MHz -> 300 kHz job as
 * the sinc^6, in two cheaper stages instead of one long table walk.
 *
 *   stage 1  sinc^4 over bytes, R=8: four 256-entry int16 tables, four lookups
 *            per byte, giving 600 kHz samples of at most +-4096. Table lookups,
 *            so SIMD cannot help here, and this is what bounds the whole filter
 *            at about 160 cycles per output frame for two channels;
 *   stage 2  24-tap FIR, decimate by 2 (pdm_stage2_scalar.c, or the S3 vector
 *            unit in pdm_stage2_simd.c).
 *
 * A different filter from the sinc^6, not a restructuring of it: a bit-exact
 * split would still need twelve lookups per output.
 */
#include <stdlib.h>
#include <string.h>

#include "pdm_decimator.h"
#include "pdm_decimator_priv.h"

#define S1_HIST PDM_STAGE2_TAPS     /* stage-1 samples carried between calls */

struct pdm_twostage {
    int channels;
    int16_t stage1[4][256];             /* by byte age, 0 newest */
    uint8_t prev[PDM_DECIMATOR_MAX_CHANNELS][3];   /* last three bytes, newest first */
    int16_t *buf;                       /* [channels][S1_HIST + cap + PDM_STAGE2_PAD] */
    size_t cap;                         /* new stage-1 samples per channel it can hold */
    int16_t hist[PDM_DECIMATOR_MAX_CHANNELS][S1_HIST] __attribute__((aligned(16)));
    int first_shift;                    /* which byte of a word is older: 8 or 0 */
    bool simd;
};

esp_err_t pdm_twostage_create(int channels, bool lsb_first, bool simd, pdm_twostage_t **out)
{
    *out = NULL;
    pdm_twostage_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    d->channels = channels;
    d->simd = simd && PDM_HAVE_SIMD;
    /* MSB-first: the high byte is the older one. LSB-first: the low byte is. */
    d->first_shift = lsb_first ? 0 : 8;

    /* box8 convolved four times: 29 taps, gain 8^4 = 4096. */
    int h[32] = {0};
    h[0] = 1;
    int len = 1;
    for (int stage = 0; stage < 4; stage++) {
        int t[32] = {0};
        for (int i = 0; i < len; i++) {
            for (int j = 0; j < 8; j++) {
                t[i + j] += h[i];
            }
        }
        len += 7;
        memcpy(h, t, sizeof(h));
    }
    /* Within a byte, bit 0 is the newest sample for MSB-first data and the
     * oldest for LSB-first, so the weight a bit gets mirrors. */
    for (int a = 0; a < 4; a++) {
        for (int v = 0; v < 256; v++) {
            int sum = 0;
            for (int bit = 0; bit < 8; bit++) {
                int pos = 8 * a + (lsb_first ? 7 - bit : bit);
                int w = pos < len ? h[pos] : 0;
                sum += (v & (1 << bit)) ? w : -w;
            }
            d->stage1[a][v] = (int16_t)sum;
        }
    }
    *out = d;
    return ESP_OK;
}

void pdm_twostage_destroy(pdm_twostage_t *d)
{
    if (d) {
        free(d->buf);
        free(d);
    }
}

bool pdm_twostage_uses_simd(const pdm_twostage_t *d)
{
    return d->simd;
}

esp_err_t pdm_twostage_reserve(pdm_twostage_t *d, size_t max_frames)
{
    const size_t n1 = 2 * max_frames;           /* stage-1 samples per channel */
    if (n1 <= d->cap) {
        return ESP_OK;
    }
    free(d->buf);
    d->buf = pdm_alloc_aligned16((size_t)d->channels * (S1_HIST + n1 + PDM_STAGE2_PAD) *
                                 sizeof(int16_t));
    if (!d->buf) {
        d->cap = 0;
        return ESP_ERR_NO_MEM;
    }
    d->cap = n1;
    return ESP_OK;
}

esp_err_t pdm_twostage_process(pdm_twostage_t *d, const uint16_t *raw, size_t frames,
                               int16_t *out, size_t *produced)
{
    const int ch = d->channels;
    const size_t n1 = 2 * frames;
    *produced = 0;
    if (frames == 0) {
        return ESP_OK;      /* nothing to do, and no work buffer may exist yet */
    }

    esp_err_t err = pdm_twostage_reserve(d, frames);
    if (err != ESP_OK) {
        return err;
    }
    const size_t cstride = S1_HIST + d->cap + PDM_STAGE2_PAD;
    const int sh0 = d->first_shift, sh1 = 8 - d->first_shift;

    for (int c = 0; c < ch; c++) {
        int16_t *x = d->buf + (size_t)c * cstride;
        memcpy(x, d->hist[c], S1_HIST * sizeof(int16_t));
        int16_t *s1 = x + S1_HIST;

        /* p1..p3 are the three bytes before the current one, newest first. */
        uint8_t p1 = d->prev[c][0], p2 = d->prev[c][1], p3 = d->prev[c][2];
        const int16_t *t0 = d->stage1[0], *t1 = d->stage1[1];
        const int16_t *t2 = d->stage1[2], *t3 = d->stage1[3];
        for (size_t f = 0; f < frames; f++) {
            uint16_t w = raw[f * (size_t)ch + (size_t)c];
            uint8_t b0 = (uint8_t)(w >> sh0);     /* the older byte of the word */
            uint8_t b1 = (uint8_t)(w >> sh1);     /* the newer */
            s1[2 * f] = (int16_t)(t0[b0] + t1[p1] + t2[p2] + t3[p3]);
            s1[2 * f + 1] = (int16_t)(t0[b1] + t1[b0] + t2[p1] + t3[p2]);
            p3 = p1;
            p2 = b0;
            p1 = b1;
        }
        d->prev[c][0] = p1;
        d->prev[c][1] = p2;
        d->prev[c][2] = p3;

        /* An output after every second stage-1 sample: the newer byte of a word. */
#if PDM_HAVE_SIMD
        if (d->simd) {
            pdm_stage2_simd(s1, n1, out + c, (size_t)ch);
        } else
#endif
        {
            pdm_stage2_scalar(s1, n1, out + c, (size_t)ch);
        }
        memcpy(d->hist[c], s1 + n1 - S1_HIST, S1_HIST * sizeof(int16_t));
    }
    *produced = n1 / 2;
    return ESP_OK;
}
