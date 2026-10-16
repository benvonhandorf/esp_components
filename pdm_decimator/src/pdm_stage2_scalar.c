/*
 * Stage 2 of the two-stage decimator, portable C. This is the reference the
 * vector version in pdm_stage2_simd.c is held to, and the only version on a
 * chip without that unit. Keep it dull and obviously right.
 */
#include "pdm_decimator_priv.h"

/* Designed with a Kaiser window, beta 6, 95 kHz cut at the 600 kHz stage-1
 * rate, so that with the sinc^4 ahead of it the response follows the sinc^6 by 16
 * to within 0.5 dB at low frequency and sits about 3 dB above it at 80 kHz,
 * where the sinc^6 has drooped 6 dB and this has drooped 3. Symmetric. 16-byte aligned for the vector
 * loads in pdm_stage2_simd.c. */
const int16_t pdm_stage2_coef[PDM_STAGE2_TAPS] __attribute__((aligned(16))) = {
    -12, -40, -3, 176, 348, 113, -709, -1459, -767, 2227, 6612, 9899,
    9897, 6612, 2227, -767, -1459, -709, 113, 348, 176, -3, -40, -12,
};

void pdm_stage2_scalar(const int16_t *s1, size_t n1, int16_t *out, size_t out_stride)
{
    size_t o = 0;
    for (size_t j = 1; j < n1; j += 2) {
        const int16_t *win = s1 + j - (PDM_STAGE2_TAPS - 1);
        int32_t acc = 0;
        for (int k = 0; k < PDM_STAGE2_TAPS; k++) {
            acc += (int32_t)win[k] * pdm_stage2_coef[k];
        }
        out[o * out_stride] = (int16_t)pdm_clamp16(acc >> PDM_STAGE2_SHIFT);
        o++;
    }
}
