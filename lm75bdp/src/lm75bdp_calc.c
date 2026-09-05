#include "lm75bdp_calc.h"

#include <math.h>

float lm75bdp_temperature_c(uint16_t raw)
{
    /* Arithmetic shift on a signed value sign-extends, which is what makes a
     * sub-zero reading come out negative. */
    return (float)((int16_t)raw >> 5) * 0.125f;
}

uint16_t lm75bdp_encode_threshold(float temp_c)
{
    /*
     * Rounded, not truncated. Truncation moves toward zero, so -0.7 C would
     * become -0.5 rather than -1.0 -- a threshold that sits on the wrong side of
     * the temperature it was meant to catch, and only below freezing.
     */
    int steps = (int)lroundf(temp_c * 2.0f);

    /* 9 bits signed: -256..255 steps, i.e. -128.0 to +127.5 C. Clamp rather than
     * let the mask wrap a hot limit into a cold one. */
    if (steps > 255) {
        steps = 255;
    } else if (steps < -256) {
        steps = -256;
    }

    return (uint16_t)(((uint16_t)steps & 0x01FF) << 7);
}

float lm75bdp_decode_threshold(uint16_t raw)
{
    /* Sign-extend the 9-bit field: shift it up to the top of the word, then back
     * down as a signed value. */
    int16_t steps = (int16_t)(raw & 0xFF80);
    return (float)(steps >> 7) * 0.5f;
}
