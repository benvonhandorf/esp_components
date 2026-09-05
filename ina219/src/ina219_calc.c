#include "ina219_calc.h"

#include <math.h>

/* Datasheet SBOS448G. Note this is NOT the INA226's 0.00512. */
#define CAL_CONSTANT   0.04096f
#define CURRENT_BITS   32768.0f
#define POWER_LSB_MULT 20.0f        /* the INA226 uses 25 */

#define SHUNT_LSB_V 0.00001f        /* 10 uV */
#define BUS_LSB_V   0.004f          /* 4 mV, on bits [15:3] */

/*
 * A calibration below this stores as zero once FS0 is masked, and a zero
 * calibration leaves the current and power registers at zero forever. The PGA
 * limit already implies a far higher floor -- CAL is 0.04096 * 2^15 divided by
 * the full-scale shunt drop, so 320 mV puts it at 4194 -- but the register's own
 * constraint is worth stating where it belongs.
 */
#define CAL_MIN 2.0f
#define CAL_MAX 65535.0f

/*
 * Compare the requested shunt drop against the PGA ceiling with a little slack.
 * 3.2 A across 0.1 ohm is exactly 320 mV, but none of those values is exact in
 * binary and the product lands a few ULPs above the limit. A hundredth of a
 * percent is orders below anything the part can resolve.
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

bool ina219_calibration_compute(float shunt_ohms, float max_current_a,
                                ina219_calibration_t *out)
{
    if (!out || !(shunt_ohms > 0.0f) || !(max_current_a > 0.0f) ||
        !isfinite(shunt_ohms) || !isfinite(max_current_a)) {
        return false;
    }

    /*
     * Refuse a range the front end cannot see before computing a calibration for
     * it. Without this the arithmetic succeeds and full_scale_a reports a current
     * the part will never register: 10 A across 0.1 ohm is a volt of shunt drop
     * against a 320 mV input, and everything above 3.2 A reads as 3.2 A.
     */
    if (max_current_a * shunt_ohms >
        INA219_SHUNT_FULL_SCALE_V * SHUNT_FULL_SCALE_SLACK) {
        return false;
    }

    const float current_lsb = max_current_a / CURRENT_BITS;
    const float cal = CAL_CONSTANT / (current_lsb * shunt_ohms);

    /* Equation 1 truncates. Rounding up would shrink the LSB and pull full scale
     * below the range that was asked for. */
    const float truncated = truncf(cal * (1.0f + CAL_TRUNC_EPSILON));
    if (!(truncated >= CAL_MIN) || truncated > CAL_MAX) {
        return false;
    }

    /* The part cannot hold the low bit, so do not pretend to have written it. */
    out->calibration = (uint16_t)truncated & INA219_CAL_FS0_MASK;
    /*
     * Report the LSB the register actually yields, not the one that was asked
     * for. Truncating the calibration and dropping FS0 both change it, and a
     * caller scaling by the requested value instead of this one is precisely the
     * bug that made the original read high.
     */
    out->current_lsb_a = ina219_current_lsb_for(out->calibration, shunt_ohms);
    out->power_lsb_w   = out->current_lsb_a * POWER_LSB_MULT;
    out->full_scale_a  = out->current_lsb_a * CURRENT_BITS;
    return true;
}

float ina219_current_lsb_for(uint16_t calibration, float shunt_ohms)
{
    /* What the part stores, not what was handed to it. */
    const uint16_t effective = calibration & INA219_CAL_FS0_MASK;

    if (effective == 0 || !(shunt_ohms > 0.0f)) {
        return 0.0f;
    }
    return CAL_CONSTANT / ((float)effective * shunt_ohms);
}

float ina219_shunt_volts(uint16_t raw)
{
    return (float)(int16_t)raw * SHUNT_LSB_V;
}

float ina219_bus_volts(uint16_t raw)
{
    /* Bit 2 is reserved and bits [1:0] are CNVR and OVF; none is part of the
     * value, which sits in BD12:BD0. */
    return (float)((raw >> 3) & 0x1FFF) * BUS_LSB_V;
}

float ina219_current_amps(uint16_t raw, float current_lsb_a)
{
    return (float)(int16_t)raw * current_lsb_a;
}

float ina219_power_watts(uint16_t raw, float power_lsb_w)
{
    /* The power register is unsigned: Figure 25 shows PD15:PD0, with no sign
     * bit, where the current register has CSIGN. */
    return (float)raw * power_lsb_w;
}
