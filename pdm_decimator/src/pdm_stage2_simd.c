/*
 * Stage 2 of the two-stage decimator on the ESP32-S3's vector unit (the Xtensa
 * "EE" extension, which the S3 documentation calls PIE): a 24-tap dot product
 * is three 8 x 16-bit multiply-accumulates into the 40-bit ACCX register.
 *
 * pdm_stage2_scalar.c is the reference; the two must agree to the last bit, and
 * `audio pdmbench` in the bring-up rig checks it on the chip. Nothing here can
 * be built or run on a host, which is why nothing else lives in this file.
 */
#include "pdm_decimator_priv.h"

#if PDM_HAVE_SIMD

/* x need not be aligned; c must be 16-byte aligned. Reads up to 48 bytes from x
 * rounded down to 16, which PDM_STAGE2_PAD covers. The unaligned loads work by
 * loading the two aligned vectors either side and realigning with ee.src.q
 * using the shift amount ee.ld.128.usar.ip leaves in SAR_BYTE. */
static inline int32_t dot24(const int16_t *x, const int16_t *c)
{
    int32_t lo;
    asm volatile(
        "ee.zero.accx\n"
        "ee.ld.128.usar.ip q0, %[x], 16\n"
        "ee.ld.128.usar.ip q1, %[x], 16\n"
        "ee.vld.128.ip q4, %[c], 16\n"
        "ee.src.q q5, q0, q1\n"
        "ee.vmulas.s16.accx q4, q5\n"
        "ee.ld.128.usar.ip q0, %[x], 16\n"
        "ee.vld.128.ip q4, %[c], 16\n"
        "ee.src.q q5, q1, q0\n"
        "ee.vmulas.s16.accx q4, q5\n"
        "ee.ld.128.usar.ip q1, %[x], 16\n"
        "ee.vld.128.ip q4, %[c], 16\n"
        "ee.src.q q5, q0, q1\n"
        "ee.vmulas.s16.accx q4, q5\n"
        "rur.accx_0 %[lo]\n"
        : [x] "+r"(x), [c] "+r"(c), [lo] "=r"(lo)
        :
        : "memory");
    return lo;
}

void pdm_stage2_simd(const int16_t *s1, size_t n1, int16_t *out, size_t out_stride)
{
    size_t o = 0;
    for (size_t j = 1; j < n1; j += 2) {
        const int16_t *win = s1 + j - (PDM_STAGE2_TAPS - 1);
        int32_t acc = dot24(win, pdm_stage2_coef);
        out[o * out_stride] = (int16_t)pdm_clamp16(acc >> PDM_STAGE2_SHIFT);
        o++;
    }
}

#endif /* PDM_HAVE_SIMD */
