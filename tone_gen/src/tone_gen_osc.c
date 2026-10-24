#include "tone_gen.h"

#include <math.h>

/* round(32767 * sin(pi/2 * i / 128)) for i = 0..128. */
static const int16_t quarter[129] = {
    0, 402, 804, 1206, 1608, 2009, 2410, 2811,
    3212, 3612, 4011, 4410, 4808, 5205, 5602, 5998,
    6393, 6786, 7179, 7571, 7962, 8351, 8739, 9126,
    9512, 9896, 10278, 10659, 11039, 11417, 11793, 12167,
    12539, 12910, 13279, 13645, 14010, 14372, 14732, 15090,
    15446, 15800, 16151, 16499, 16846, 17189, 17530, 17869,
    18204, 18537, 18868, 19195, 19519, 19841, 20159, 20475,
    20787, 21096, 21403, 21705, 22005, 22301, 22594, 22884,
    23170, 23452, 23731, 24007, 24279, 24547, 24811, 25072,
    25329, 25582, 25832, 26077, 26319, 26556, 26790, 27019,
    27245, 27466, 27683, 27896, 28105, 28310, 28510, 28706,
    28898, 29085, 29268, 29447, 29621, 29791, 29956, 30117,
    30273, 30424, 30571, 30714, 30852, 30985, 31113, 31237,
    31356, 31470, 31580, 31685, 31785, 31880, 31971, 32057,
    32137, 32213, 32285, 32351, 32412, 32469, 32521, 32567,
    32609, 32646, 32678, 32705, 32728, 32745, 32757, 32765,
    32767,
};

/* The sine at one of 512 points around the full cycle, from the first quadrant
 * mirrored: the second runs the table backwards, the third and fourth are the
 * first two negated. */
static inline int32_t point(uint32_t i)
{
    uint32_t offset = i & 127;
    switch ((i >> 7) & 3) {
        case 0:  return quarter[offset];
        case 1:  return quarter[128 - offset];
        case 2:  return -quarter[offset];
        default: return -quarter[128 - offset];
    }
}

int16_t tone_gen_sine(uint32_t phase)
{
    uint32_t index = phase >> 23;                    /* 9 bits: 512 points */
    int32_t frac = (int32_t)((phase >> 7) & 0xFFFF); /* the next 16 bits */
    int32_t a = point(index);
    int32_t b = point((index + 1) & 511);
    /* 16 fractional bits, not 8: with 512 points a 1/256 step in the
     * interpolation is worth about 3 LSB at full scale, more than everything
     * else in the oscillator put together. */
    return (int16_t)(a + (((b - a) * frac) >> 16));
}

uint32_t tone_gen_phase_step(double hz, uint32_t rate_hz)
{
    return (uint32_t)llrint(hz / (double)rate_hz * 4294967296.0);
}

uint32_t tone_gen_phase_step_int(uint32_t hz, uint32_t rate_hz)
{
    return (uint32_t)((((uint64_t)hz << 32) + rate_hz / 2) / rate_hz);
}

int32_t tone_gen_ramp_gain(uint32_t n, uint32_t span)
{
    if (span == 0 || n >= span) {
        return 32768;
    }
    /* A quarter turn of the sine over the span, squared: sin^2 rises smoothly
     * from 0 to 1 with no slope at either end, so neither edge clicks. The
     * square comes from the same table, so the sample loop stays integer-only. */
    uint32_t phase = (uint32_t)(((uint64_t)n << 30) / span);
    int32_t s = tone_gen_sine(phase);
    return (s * s) >> 15;
}
