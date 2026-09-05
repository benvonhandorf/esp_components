#ifndef INA226_CALC_H
#define INA226_CALC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The INA226's calibration arithmetic and identification rules, separated so
 * they can be tested off-target. Datasheet references are to SBOS547C.
 *
 * Getting this wrong does not fail: it produces readings that are plausible and
 * wrong by a constant factor, which is the hardest kind of measurement bug to
 * notice and the reason this is worth testing without hardware.
 */

/*
 * The calibration register is fifteen bits, not sixteen: Table 7-11 names the
 * field FS14:FS0 and leaves D15 unnamed. A calibration past this cannot be
 * represented, and truncating one into the register silently rescales every
 * current and power reading -- by 2.4x at the point the overflow begins.
 */
#define INA226_CAL_MAX 32767u

/*
 * The shunt input range, fixed at +/-81.92 mV (Table 7-7 and the electrical
 * table's -81.9175 to 81.92 mV). Unlike the INA219 there is no PGA to widen it,
 * so a range needing more shunt drop than this cannot be measured at all --
 * whatever the calibration register says.
 */
#define INA226_SHUNT_FULL_SCALE_V 0.08192f

/*
 * Table 7-15 splits the die ID register into DID15:4 -- the device -- and
 * RID3:0, the die revision. Only the device half identifies the part: the
 * register map footnote lists both 2260h and 2261h as an INA226 ("Die COO: 2260
 * = USA or Japan, 2261 = USA"), so comparing all sixteen bits rejects genuine
 * parts.
 */
#define INA226_DEVICE_ID 0x226u
#define INA226_MANUFACTURER_ID_TI 0x5449u

typedef struct {
    float    current_lsb_a;   /* amps per LSB actually programmed */
    float    power_lsb_w;     /* watts per LSB; fixed at 25x the current LSB */
    uint16_t calibration;     /* the value written to the calibration register */
    float    full_scale_a;    /* the largest current this LSB can represent */
} ina226_calibration_t;

/*
 * Compute the calibration register value for a sense resistor and a full-scale
 * current, per the datasheet:
 *
 *     current_lsb = max_current / 2^15                       (Eq. 2)
 *     CAL         = 0.00512 / (current_lsb * R_shunt)        (Eq. 1)
 *
 * Returns false if the arguments are not positive or finite; if max_current_a
 * across shunt_ohms needs more than the +/-81.92 mV the input can see; or if the
 * calibration does not fit the register's fifteen bits, which happens with a
 * very small shunt and a very small full-scale current and would otherwise wrap
 * into a silently wrong scale.
 */
bool ina226_calibration_compute(float shunt_ohms, float max_current_a,
                                ina226_calibration_t *out);

/*
 * The current LSB a given calibration register actually produces -- the inverse
 * of Equation 1.
 *
 * Scaling readings by the LSB that was *asked* for rather than the one the
 * register yields is a systematic error in every current and power value, and is
 * the bug the sibling INA219 driver was written to fix.
 */
float ina226_current_lsb_for(uint16_t calibration, float shunt_ohms);

/* Whether the manufacturer and die ID registers identify an INA226, ignoring the
 * die revision in RID3:0. */
bool ina226_part_matches(uint16_t manufacturer, uint16_t die);

/* Sign-extend the shunt and current registers, which are two's complement. */
float ina226_shunt_volts(uint16_t raw);
float ina226_current_amps(uint16_t raw, float current_lsb_a);

/*
 * The bus voltage register is *unsigned*: 0..36 V in 1.25 mV steps. Reading it as
 * signed works only because 36 V lands below 0x7FFF; it is wrong in principle and
 * would break on any part with a wider range.
 */
float ina226_bus_volts(uint16_t raw);
float ina226_power_watts(uint16_t raw, float power_lsb_w);

#endif /* INA226_CALC_H */
