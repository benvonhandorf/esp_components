#ifndef LM75BDP_CALC_H
#define LM75BDP_CALC_H

#include <stdint.h>

/*
 * The LM75BDP's temperature encoding, separated so it can be tested off-target.
 *
 * Two different formats in one part, both two's complement and neither
 * byte-aligned, which is exactly the kind of thing that reads correctly above
 * freezing and wrongly below it.
 */

/* Temperature register: upper 11 bits, 0.125 C per LSB. */
float lm75bdp_temperature_c(uint16_t raw);

/*
 * Threshold registers (Tos, Thyst): 9 bits, 0.5 C per LSB, left-justified into
 * bits 15:7.
 */
uint16_t lm75bdp_encode_threshold(float temp_c);
float    lm75bdp_decode_threshold(uint16_t raw);

#endif /* LM75BDP_CALC_H */
