#include "ina226_calc.h"

#include <math.h>

/* Datasheet constants. The 0.00512 is the fixed internal scaling term; the
 * power register is always 25 LSBs of current. */
#define CAL_CONSTANT   0.00512f
#define CURRENT_BITS   32768.0f     /* 2^15: the current register is signed */
#define POWER_LSB_MULT 25.0f

#define SHUNT_LSB_V 0.0000025f      /* 2.5 uV */
#define BUS_LSB_V   0.00125f        /* 1.25 mV */

/*
 * A calibration of zero leaves the current and power registers reading zero
 * forever (Table 7-1, note 2). The shunt range already implies a much higher
 * floor -- CAL is 0.00512 * 2^15 divided by the full-scale shunt drop, so
 * 81.92 mV puts it at 2048 -- but the register's own constraint belongs here.
 */
#define CAL_MIN 1.0f

/*
 * Compare the requested shunt drop against the input range with a little slack:
 * 8.192 A across 0.01 ohm is exactly 81.92 mV, but none of those values is exact
 * in binary and the product can land a few ULPs above the limit. A hundredth of
 * a percent is orders below anything the part can resolve.
 */
#define SHUNT_FULL_SCALE_SLACK 1.0001f

/*
 * Truncate, but first nudge a quotient that sits within float rounding error of
 * an integer up onto it. The operands are floats supplied by the caller, so an
 * exactly representable answer need not come out exact: 32.768 A across 2 mOhm
 * is exactly CAL 2560 -- the datasheet's own worked example -- yet the quotient
 * lands at 2559.9999 and would truncate to 2559. A hundred-thousandth is far
 * below one calibration count of significance and cannot promote a value that
 * was genuinely short of the next integer.
 */
#define CAL_TRUNC_EPSILON 1.0e-5f

bool ina226_calibration_compute(float shunt_ohms, float max_current_a,
                                ina226_calibration_t *out)
{
    if (!out || !(shunt_ohms > 0.0f) || !(max_current_a > 0.0f) ||
        !isfinite(shunt_ohms) || !isfinite(max_current_a)) {
        return false;
    }

    /*
     * Refuse a range the input cannot see before computing a calibration for it.
     * There is no PGA on this part: the shunt range is fixed. Without this the
     * arithmetic succeeds and full_scale_a reports a current the part will never
     * register -- 32.768 A across 0.01 ohm is 328 mV against an 81.92 mV input,
     * and everything above 8.192 A reads as 8.192 A.
     */
    if (max_current_a * shunt_ohms >
        INA226_SHUNT_FULL_SCALE_V * SHUNT_FULL_SCALE_SLACK) {
        return false;
    }

    const float current_lsb = max_current_a / CURRENT_BITS;
    const float cal = CAL_CONSTANT / (current_lsb * shunt_ohms);

    /*
     * Truncated, so the LSB never comes out smaller than asked for and the
     * requested range stays reachable. Equation 1 does not say which way to go;
     * rounding up would shrink the LSB and pull full scale just under the range
     * the caller asked for. Either way there is no scale error, because the LSB
     * reported below is derived from the register rather than from the request.
     */
    const float truncated = truncf(cal * (1.0f + CAL_TRUNC_EPSILON));
    if (!(truncated >= CAL_MIN) || truncated > (float)INA226_CAL_MAX) {
        return false;
    }

    out->calibration = (uint16_t)truncated;
    /*
     * Report the LSB the register actually yields, not the one that was asked
     * for. A caller scaling by the requested value instead of this one is a
     * systematic error in every reading -- the bug the sibling INA219 driver
     * exists to fix.
     */
    out->current_lsb_a = ina226_current_lsb_for(out->calibration, shunt_ohms);
    out->power_lsb_w   = out->current_lsb_a * POWER_LSB_MULT;
    out->full_scale_a  = out->current_lsb_a * CURRENT_BITS;
    return true;
}

float ina226_current_lsb_for(uint16_t calibration, float shunt_ohms)
{
    if (calibration == 0 || !(shunt_ohms > 0.0f)) {
        return 0.0f;
    }
    return CAL_CONSTANT / ((float)calibration * shunt_ohms);
}

bool ina226_part_matches(uint16_t manufacturer, uint16_t die)
{
    /* The low nibble is the die revision, not part of the identity. */
    return manufacturer == INA226_MANUFACTURER_ID_TI &&
           (uint16_t)(die >> 4) == INA226_DEVICE_ID;
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
