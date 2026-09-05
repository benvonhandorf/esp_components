#include "ina219_calc.h"

#include <math.h>

/* Datasheet SBOS448. Note this is NOT the INA226's 0.00512. */
#define CAL_CONSTANT   0.04096f
#define CURRENT_BITS   32768.0f
#define POWER_LSB_MULT 20.0f        /* the INA226 uses 25 */

#define SHUNT_LSB_V 0.00001f        /* 10 uV */
#define BUS_LSB_V   0.004f          /* 4 mV, on bits [15:3] */

bool ina219_calibration_compute(float shunt_ohms, float max_current_a,
                                ina219_calibration_t *out)
{
    if (!out || !(shunt_ohms > 0.0f) || !(max_current_a > 0.0f) ||
        !isfinite(shunt_ohms) || !isfinite(max_current_a)) {
        return false;
    }

    const float current_lsb = max_current_a / CURRENT_BITS;
    const float cal = CAL_CONSTANT / (current_lsb * shunt_ohms);

    const float rounded = roundf(cal);
    if (!(rounded >= 1.0f) || rounded > 65535.0f) {
        return false;
    }

    out->calibration = (uint16_t)rounded;
    /*
     * Report the LSB the register actually yields, not the one that was asked
     * for. Rounding the calibration changes it slightly, and a caller scaling by
     * the requested value instead of this one is precisely the bug that made the
     * original read high.
     */
    out->current_lsb_a = ina219_current_lsb_for(out->calibration, shunt_ohms);
    out->power_lsb_w   = out->current_lsb_a * POWER_LSB_MULT;
    out->full_scale_a  = out->current_lsb_a * CURRENT_BITS;
    return true;
}

float ina219_current_lsb_for(uint16_t calibration, float shunt_ohms)
{
    if (calibration == 0 || !(shunt_ohms > 0.0f)) {
        return 0.0f;
    }
    return CAL_CONSTANT / ((float)calibration * shunt_ohms);
}

float ina219_shunt_volts(uint16_t raw)
{
    return (float)(int16_t)raw * SHUNT_LSB_V;
}

float ina219_bus_volts(uint16_t raw)
{
    /* Bits [2:0] are CNVR and OVF, not part of the value. */
    return (float)((raw >> 3) & 0x1FFF) * BUS_LSB_V;
}

float ina219_current_amps(uint16_t raw, float current_lsb_a)
{
    return (float)(int16_t)raw * current_lsb_a;
}

float ina219_power_watts(uint16_t raw, float power_lsb_w)
{
    return (float)raw * power_lsb_w;
}
