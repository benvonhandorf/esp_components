/*
 * The format half of audio_stream: plain C, no hardware, host-tested.
 * See audio_stream.h.
 */

#include "audio_stream.h"

#include <stdlib.h>
#include <string.h>

#include "pdm_decimator.h"

static const int raw_ratios[] = {64, 48, 32, 16};
#define RAW_RATIO_COUNT   (sizeof(raw_ratios) / sizeof(raw_ratios[0]))
#define RAW_RATIO_DEFAULT 64

const int *audio_stream_raw_ratios(size_t *count)
{
    if (count) {
        *count = RAW_RATIO_COUNT;
    }
    return raw_ratios;
}

bool audio_stream_source_valid(const audio_stream_source_t *src)
{
    if (!src || !src->read || src->channels == 0) {
        return false;
    }
    if (src->pdm_raw) {
        /* One 16-bit word of the bit stream per channel, which is also the size of one
         * decimated frame: the runner relies on both being the same. */
        return src->pdm_clk_hz > 0 && src->channels <= PDM_DECIMATOR_MAX_CHANNELS &&
               src->slot_bytes == 2;
    }
    return src->rate_hz > 0 && (src->slot_bytes == 2 || src->slot_bytes == 4);
}

static esp_err_t refuse(audio_stream_why_t *why, esp_err_t code)
{
    why->code = code;
    return code;
}

esp_err_t audio_stream_parse_channels(const char *text, int nch_available, uint8_t *out,
                                      int *count, audio_stream_why_t *why)
{
    audio_stream_why_t local;
    if (!why) {
        why = &local;
    }
    memset(why, 0, sizeof *why);
    why->channels_available = nch_available;

    int n = 0;
    const char *p = text;
    while (p && *p) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || (*end != ',' && *end != '\0') || v < 0 || v > 255) {
            return refuse(why, AUDIO_STREAM_ERR_BAD_LIST);
        }
        if (v >= nch_available) {
            why->channel = (int)v;
            return refuse(why, AUDIO_STREAM_ERR_BAD_CHANNEL);
        }
        for (int i = 0; i < n; i++) {
            if (out[i] == v) {
                why->channel = (int)v;
                return refuse(why, AUDIO_STREAM_ERR_DUP_CHANNEL);
            }
        }
        if (n >= AUDIO_STREAM_MAX_CHANNELS) {
            return refuse(why, AUDIO_STREAM_ERR_BAD_LIST);
        }
        out[n++] = (uint8_t)v;
        p = (*end == ',') ? end + 1 : end;
        if (*end == ',' && *p == '\0') {      /* "0,1," */
            return refuse(why, AUDIO_STREAM_ERR_BAD_LIST);
        }
    }
    if (n == 0) {
        return refuse(why, AUDIO_STREAM_ERR_BAD_LIST);
    }
    *count = n;
    why->code = ESP_OK;
    return ESP_OK;
}

esp_err_t audio_stream_resolve(const audio_stream_source_t *src, const audio_stream_want_t *want,
                               audio_stream_fmt_t *fmt, audio_stream_why_t *why)
{
    audio_stream_why_t local;
    if (!why) {
        why = &local;
    }
    memset(why, 0, sizeof *why);
    if (!src || !want || !fmt) {
        return refuse(why, ESP_ERR_INVALID_ARG);
    }
    memset(fmt, 0, sizeof *fmt);
    if (!audio_stream_source_valid(src)) {
        return refuse(why, AUDIO_STREAM_ERR_NO_SOURCE);
    }
    why->src_rate = src->rate_hz;
    why->want_rate = want->rate;
    why->src_bits = src->data_bits;
    why->channels_available = src->channels;

    uint32_t rate = src->rate_hz;
    int ratio = 0;
    if (src->pdm_raw) {
        for (size_t i = 0; i < RAW_RATIO_COUNT; i++) {
            why->raw_rates[i] = src->pdm_clk_hz / (uint32_t)raw_ratios[i];
        }
        /* No rate asked for: the standard oversampling. Otherwise the rate has to be the
         * clock over one of the ratios, exactly. */
        ratio = want->rate ? 0 : RAW_RATIO_DEFAULT;
        for (size_t i = 0; want->rate && i < RAW_RATIO_COUNT; i++) {
            if (src->pdm_clk_hz % (uint32_t)raw_ratios[i] == 0 &&
                src->pdm_clk_hz / (uint32_t)raw_ratios[i] == want->rate) {
                ratio = raw_ratios[i];
            }
        }
        if (ratio == 0) {
            return refuse(why, AUDIO_STREAM_ERR_BAD_RATE);
        }
        rate = src->pdm_clk_hz / (uint32_t)ratio;
    } else if (want->rate && want->rate != src->rate_hz) {
        return refuse(why, AUDIO_STREAM_ERR_BAD_RATE);
    }

    int nch = want->nch ? want->nch : src->channels;
    if (nch > AUDIO_STREAM_MAX_CHANNELS) {
        why->channel = nch;
        return refuse(why, AUDIO_STREAM_ERR_BAD_CHANNEL);
    }
    for (int i = 0; i < nch; i++) {
        int c = want->nch ? want->chan[i] : i;
        if (c >= src->channels) {          /* the microphone changed since the request */
            why->channel = c;
            return refuse(why, AUDIO_STREAM_ERR_BAD_CHANNEL);
        }
        fmt->chan[i] = (uint8_t)c;
    }

    uint8_t bits = want->bits ? want->bits : 16;
    if ((bits != 16 && bits != 24) || (bits == 24 && src->data_bits < 24)) {
        return refuse(why, AUDIO_STREAM_ERR_BAD_BITS);
    }

    fmt->rate_hz = rate;
    fmt->channels = (uint8_t)nch;
    fmt->bytes = (uint8_t)(bits / 8);
    fmt->ratio = ratio;
    why->code = ESP_OK;
    return ESP_OK;
}

/* ---- conversion ------------------------------------------------------------------------ */

/* One input slot to the output width. `slot` is 2 or 4 bytes with the data left-justified;
 * the output is 16- or 24-bit little-endian. */
static inline uint8_t *put_sample(uint8_t *out, const uint8_t *in, size_t slot, uint8_t out_bytes)
{
    if (slot == 2) {
        int16_t v;
        memcpy(&v, in, 2);
        if (out_bytes == 3) {                 /* left-justified: nothing below the 16 bits */
            out[0] = 0;
            memcpy(out + 1, &v, 2);
            return out + 3;
        }
        memcpy(out, &v, 2);
        return out + 2;
    }
    int32_t v;
    memcpy(&v, in, 4);
    if (out_bytes == 2) {
        int16_t s = (int16_t)(v >> 16);
        memcpy(out, &s, 2);
        return out + 2;
    }
    int32_t s = v >> 8;                       /* the top 24 bits */
    out[0] = (uint8_t)s;
    out[1] = (uint8_t)(s >> 8);
    out[2] = (uint8_t)(s >> 16);
    return out + 3;
}

size_t audio_stream_convert(const audio_stream_fmt_t *fmt, size_t slot_bytes, int src_channels,
                            const void *in, size_t frames, uint8_t *out)
{
    const uint8_t *src = in;
    const size_t frame_bytes = (size_t)src_channels * slot_bytes;
    uint8_t *dst = out;
    for (size_t f = 0; f < frames; f++) {
        const uint8_t *frame = src + f * frame_bytes;
        for (int c = 0; c < fmt->channels; c++) {
            dst = put_sample(dst, frame + (size_t)fmt->chan[c] * slot_bytes, slot_bytes, fmt->bytes);
        }
    }
    return (size_t)(dst - out);
}

/* ---- WAV ------------------------------------------------------------------------------- */

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static uint8_t *put_tag(uint8_t *p, const char *tag)
{
    memcpy(p, tag, 4);
    return p + 4;
}

size_t audio_stream_wav_header(const audio_stream_fmt_t *fmt, uint8_t *buf)
{
    const uint16_t bits = (uint16_t)(fmt->bytes * 8);
    const uint16_t block = (uint16_t)(fmt->channels * fmt->bytes);
    const bool plain = fmt->channels <= 2 && fmt->bytes == 2;
    uint8_t *p = buf;

    p = put_tag(p, "RIFF");
    p = put32(p, 0xFFFFFFFFu);
    p = put_tag(p, "WAVE");
    p = put_tag(p, "fmt ");
    p = put32(p, plain ? 16 : 40);
    p = put16(p, plain ? 0x0001 : 0xFFFE);               /* PCM, or EXTENSIBLE */
    p = put16(p, fmt->channels);
    p = put32(p, fmt->rate_hz);
    p = put32(p, fmt->rate_hz * block);
    p = put16(p, block);
    p = put16(p, bits);
    if (!plain) {
        static const uint8_t pcm_guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                             0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        p = put16(p, 22);                                /* cbSize */
        p = put16(p, bits);                              /* valid bits per sample */
        p = put32(p, fmt->channels == 1 ? 0x4 : fmt->channels == 2 ? 0x3 : 0);
        memcpy(p, pcm_guid, sizeof pcm_guid);
        p += sizeof pcm_guid;
    }
    p = put_tag(p, "data");
    p = put32(p, 0xFFFFFFFFu);
    return (size_t)(p - buf);
}
