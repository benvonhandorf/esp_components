#ifndef INA219_CALC_H
#define INA219_CALC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The INA219's calibration arithmetic, separated so it can be tested off-target.
 *
 * The scaling term is 0.04096, which is the INA219's -- the INA226 uses 0.00512.
 * Mixing them up does not fail: it produces readings that are wrong by a constant
 * factor and look entirely reasonable, which is exactly what happened to the
 * driver this was extracted from. See the test.
 */

typedef struct {
    float    current_lsb_a;   /* amps per LSB actually programmed */
    float    power_lsb_w;     /* watts per LSB; fixed at 20x the current LSB */
    uint16_t calibration;     /* the value written to the calibration register */
    float    full_scale_a;    /* the largest current this LSB can represent */
} ina219_calibration_t;

/*
 *     current_lsb = max_current / 2^15
 *     CAL         = 0.04096 / (current_lsb * R_shunt)
 *
 * Returns false if the arguments are not positive or the result does not fit the
 * 16-bit register.
 */
bool ina219_calibration_compute(float shunt_ohms, float max_current_a,
                                ina219_calibration_t *out);

/*
 * The current LSB a given calibration register actually produces.
 *
 * The inverse of the above, and the reason the original's readings were wrong:
 * its calibration register and the LSB its read path assumed did not agree.
 */
float ina219_current_lsb_for(uint16_t calibration, float shunt_ohms);

/* Shunt: 10 uV per bit, two's complement, regardless of the PGA setting. */
float ina219_shunt_volts(uint16_t raw);
/* Bus: bits [15:3] only, 4 mV per bit. The low three bits are status flags. */
float ina219_bus_volts(uint16_t raw);
float ina219_current_amps(uint16_t raw, float current_lsb_a);
float ina219_power_watts(uint16_t raw, float power_lsb_w);

#endif /* INA219_CALC_H */
