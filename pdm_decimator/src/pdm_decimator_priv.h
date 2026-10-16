#pragma once

/* Internals shared by the sources of this component. Nothing here is public. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The chips with a vector unit this component has code for. The S3's is the
 * Xtensa "EE" extension; the P4's is a different instruction set and is not
 * covered, so it takes the portable path like everything else. */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define PDM_HAVE_SIMD 1
#else
#define PDM_HAVE_SIMD 0
#endif

/* 16-byte-aligned scratch from internal RAM on the device, plain aligned_alloc
 * on the host. Release with free(). */
void *pdm_alloc_aligned16(size_t bytes);

static inline int32_t pdm_clamp16(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

/* ---- sixth-order sinc (pdm_sinc6.c) ---- */

typedef struct pdm_sinc6 pdm_sinc6_t;

esp_err_t pdm_sinc6_create(int channels, int ratio, bool lsb_first, pdm_sinc6_t **out);
void pdm_sinc6_destroy(pdm_sinc6_t *d);
size_t pdm_sinc6_process(pdm_sinc6_t *d, const uint16_t *raw, size_t frames, int16_t *out);

/* ---- two-stage (pdm_twostage.c) ---- */

#define PDM_STAGE2_TAPS 24      /* three 8-lane vectors */
#define PDM_STAGE2_SHIFT 12     /* stage 1 has gain 8^4 = 4096 */

typedef struct pdm_twostage pdm_twostage_t;

esp_err_t pdm_twostage_create(int channels, bool lsb_first, bool simd, pdm_twostage_t **out);
void pdm_twostage_destroy(pdm_twostage_t *d);
esp_err_t pdm_twostage_reserve(pdm_twostage_t *d, size_t max_frames);
esp_err_t pdm_twostage_process(pdm_twostage_t *d, const uint16_t *raw, size_t frames,
                               int16_t *out, size_t *produced);
bool pdm_twostage_uses_simd(const pdm_twostage_t *d);

/*
 * Stage 2: a 24-tap FIR decimating by 2, over one channel's stage-1 samples.
 *
 * s1 points at the first NEW stage-1 sample and PDM_STAGE2_TAPS - 1 samples of
 * history precede it. n1 is the number of new samples (even). Outputs land at
 * out[o * out_stride] for o = 0 .. n1/2 - 1. The window for output o ends on
 * s1[2o + 1].
 *
 * There are two implementations of exactly this, a portable one and one for the
 * S3's vector unit, and they must agree to the last bit. The vector one reads
 * up to 48 bytes from a 16-byte-rounded-down window start, so the buffer behind
 * s1 needs PDM_STAGE2_PAD samples of slack past its end. pdm_stage2_coef is
 * 16-byte aligned for the same reason.
 */
#define PDM_STAGE2_PAD 32

extern const int16_t pdm_stage2_coef[PDM_STAGE2_TAPS];

void pdm_stage2_scalar(const int16_t *s1, size_t n1, int16_t *out, size_t out_stride);
#if PDM_HAVE_SIMD
void pdm_stage2_simd(const int16_t *s1, size_t n1, int16_t *out, size_t out_stride);
#endif
