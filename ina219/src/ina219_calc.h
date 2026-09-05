#ifndef INA219_CALC_H
#define INA219_CALC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The INA219's calibration arithmetic, separated so it can be tested off-target.
 * Datasheet references are to SBOS448G (December 2015).
 *
 * The scaling term is 0.04096, which is the INA219's -- the INA226 uses 0.00512.
 * Mixing them up does not fail: it produces readings that are wrong by a constant
 * factor and look entirely reasonable, which is exactly what happened to the
 * driver this was extracted from. See the test.
 */

/*
 * Bit 0 of the calibration register (FS0) is void. Figure 27 types it R-0 where
 * FS15:FS1 are R/W-0, and the note reads: "FS0 is a void bit and will always be
 * 0. It is not possible to write a 1 to FS0. CALIBRATION is the value stored in
 * FS15:FS1."
 *
 * So the part silently stores an odd calibration as one less, and two things
 * follow. A read-back used to confirm the part is present will not match what
 * was written -- which is about half of all otherwise valid configurations,
 * including the datasheet's own 2 mOhm / 15 A design example. And the current
 * LSB computed from the value written is not the one the part actually uses.
 * Every calibration this module produces is therefore already even.
 */
#define INA219_CAL_FS0_MASK 0xFFFEu

/*
 * The widest shunt range the part offers, PGA /8 (Table 4), which is what the
 * driver programs. Past this the input saturates whatever the calibration says,
 * so a range needing more shunt drop than this cannot be measured at all.
 */
#define INA219_SHUNT_FULL_SCALE_V 0.320f

typedef struct {
    float    current_lsb_a;   /* amps per LSB actually programmed */
    float    power_lsb_w;     /* watts per LSB; fixed at 20x the current LSB */
    uint16_t calibration;     /* written to the calibration register; always even */
    /*
     * The largest current the current register can represent. This is the
     * register's span; because the calibration is truncated and then made even
     * it can sit a fraction of one calibration step above the PGA's ceiling, but
     * never meaningfully beyond it -- a request that would has been refused.
     */
    float    full_scale_a;
} ina219_calibration_t;

/*
 *     current_lsb = max_current / 2^15                              (Eq. 2)
 *     CAL         = trunc(0.04096 / (current_lsb * R_shunt))        (Eq. 1)
 *
 * then masked even for FS0, above. Equation 1 truncates rather than rounds, so
 * the LSB never comes out smaller than asked for and the requested range stays
 * reachable.
 *
 * Returns false if the arguments are not positive or finite; if max_current_a
 * across shunt_ohms would need more than the PGA can see; or if the result does
 * not fit the 16-bit register or would leave it below 2 -- a calibration of zero
 * leaves the current and power registers reading zero forever (Table 2, note 2).
 */
bool ina219_calibration_compute(float shunt_ohms, float max_current_a,
                                ina219_calibration_t *out);

/*
 * The current LSB a given calibration register actually produces, FS0 included:
 * an odd argument is treated as the even value the part would really store.
 *
 * The inverse of the above, and the reason the original's readings were wrong:
 * its calibration register and the LSB its read path assumed did not agree.
 */
float ina219_current_lsb_for(uint16_t calibration, float shunt_ohms);

/* Shunt: 10 uV per bit, two's complement, sign-extended to 16 bits at every PGA
 * setting (section 8.6.3.1), so this is right regardless of the gain. */
float ina219_shunt_volts(uint16_t raw);
/* Bus: bits [15:3] only, 4 mV per bit. Bit 2 is reserved and bits [1:0] are the
 * CNVR and OVF flags. */
float ina219_bus_volts(uint16_t raw);
float ina219_current_amps(uint16_t raw, float current_lsb_a);
float ina219_power_watts(uint16_t raw, float power_lsb_w);

#endif /* INA219_CALC_H */
