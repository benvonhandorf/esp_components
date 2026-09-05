#ifndef INA226_CALC_H
#define INA226_CALC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The INA226's calibration arithmetic, separated so it can be tested off-target.
 *
 * Getting this wrong does not fail: it produces readings that are plausible and
 * wrong by a constant factor, which is the hardest kind of measurement bug to
 * notice and the reason this is worth testing without hardware.
 */

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
 *     current_lsb = max_current / 2^15
 *     CAL         = 0.00512 / (current_lsb * R_shunt)
 *
 * Returns false if the arguments are not positive or the result does not fit in
 * the 16-bit register -- which happens with a very small shunt and a very small
 * full-scale current, and would otherwise wrap into a silently wrong scale.
 */
bool ina226_calibration_compute(float shunt_ohms, float max_current_a,
                                ina226_calibration_t *out);

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
