#include "ina226_calc.h"

#include <math.h>

/* Datasheet constants. The 0.00512 is the fixed internal scaling term; the
 * power register is always 25 LSBs of current. */
#define CAL_CONSTANT   0.00512f
#define CURRENT_BITS   32768.0f     /* 2^15: the current register is signed */
#define POWER_LSB_MULT 25.0f

#define SHUNT_LSB_V 0.0000025f      /* 2.5 uV */
#define BUS_LSB_V   0.00125f        /* 1.25 mV */

bool ina226_calibration_compute(float shunt_ohms, float max_current_a,
                                ina226_calibration_t *out)
{
    if (!out || !(shunt_ohms > 0.0f) || !(max_current_a > 0.0f) ||
        !isfinite(shunt_ohms) || !isfinite(max_current_a)) {
        return false;
    }

    const float current_lsb = max_current_a / CURRENT_BITS;
    const float cal = CAL_CONSTANT / (current_lsb * shunt_ohms);

    /* Rounded, not truncated: truncation biases every reading low by up to one
     * LSB of calibration, which is a systematic scale error rather than noise. */
    const float rounded = roundf(cal);
    if (!(rounded >= 1.0f) || rounded > 65535.0f) {
        return false;
    }

    out->current_lsb_a = current_lsb;
    out->power_lsb_w   = current_lsb * POWER_LSB_MULT;
    out->calibration   = (uint16_t)rounded;
    out->full_scale_a  = current_lsb * CURRENT_BITS;
    return true;
}

float ina226_shunt_volts(uint16_t raw)
{
    return (float)(int16_t)raw * SHUNT_LSB_V;
}

float ina226_current_amps(uint16_t raw, float current_lsb_a)
{
    return (float)(int16_t)raw * current_lsb_a;
}

float ina226_bus_volts(uint16_t raw)
{
    /* Unsigned, deliberately: see the header. */
    return (float)raw * BUS_LSB_V;
}

float ina226_power_watts(uint16_t raw, float power_lsb_w)
{
    return (float)raw * power_lsb_w;
}
