#ifndef ES8311_CALC_H
#define ES8311_CALC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The arithmetic of the ES8311 that fails silently: clock dividers and the
 * volume scale. No I2C and no ESP-IDF beyond esp_err.h, so it is tested on a
 * host (`make -C es8311/test`). A wrong divider does not error -- it plays at
 * the wrong pitch or not at all -- which is why it lives apart from the driver.
 */

#define ESP_ERR_ES8311_BASE              0x40000
/* The chip ID registers did not match. */
#define ESP_ERR_ES8311_WRONG_PART        (ESP_ERR_ES8311_BASE + 1)
/* No pre-divider / pre-multiplier pair turns the clock into 256 x the sample rate. */
#define ESP_ERR_ES8311_UNSUPPORTED_CLOCK (ESP_ERR_ES8311_BASE + 2)

/* The part is clocked at 256 x the sample rate internally; everything else in
 * the clock tree here is built around that single ratio. */
#define ES8311_MCLK_MULTIPLE 256

#define ES8311_RATE_MIN_HZ 8000
#define ES8311_RATE_MAX_HZ 48000

typedef struct {
    uint8_t pre_div;        /* 1..8: divides the incoming clock */
    uint8_t pre_mult;       /* 1, 2, 4 or 8: multiplies it afterwards */
    uint32_t internal_hz;   /* what the converters actually run from */
} es8311_clock_plan_t;

/*
 * Find the pre-divider and pre-multiplier that turn `clk_in_hz` -- the MCLK
 * pin's frequency, or the bit clock when there is no MCLK pin -- into 256 x
 * `rate_hz`. The smallest divider wins, which reproduces Espressif's published
 * coefficient rows wherever they use this ratio.
 *
 * The pre-multiplier is what makes a part with no MCLK pin usable at all:
 * 16-bit stereo at 8 kHz has a 256 kHz bit clock, and x8 brings that to
 * 2.048 MHz. Returns ESP_ERR_ES8311_UNSUPPORTED_CLOCK when no pair reaches the
 * target (a bit clock of 1.5 MHz at 32 kHz would need x5.33), and
 * ESP_ERR_INVALID_ARG for a rate outside ES8311_RATE_MIN_HZ..MAX_HZ.
 */
esp_err_t es8311_clock_plan(uint32_t clk_in_hz, uint32_t rate_hz,
                            es8311_clock_plan_t *out);

/* Register 0x02: pre-divider in bits 7:5 (value - 1), pre-multiplier in 4:3
 * (log2). Only those five bits; the caller keeps the low three. */
uint8_t es8311_reg02_field(const es8311_clock_plan_t *plan);

/* Register 0x09 / 0x0A word length field (bits 4:2): 16, 18, 20, 24 or 32.
 * Returns false for any other width. */
bool es8311_word_length_field(uint8_t bits, uint8_t *field);

/*
 * DAC volume, register 0x32. Half-decibel steps: 0x00 is -95.5 dB, 0xBF is
 * 0 dB and 0xFF is +32 dB. Anything above ES8311_DAC_CODE_0DB is gain, which
 * a bring-up rig has no business applying by accident.
 */
#define ES8311_DAC_CODE_0DB 0xBF
#define ES8311_DAC_CODE_MIN 0x00

/* Tenths of a dB, so no floating point: (code - 0xBF) * 5. */
int es8311_dac_code_to_db_x10(uint8_t code);
/* Nearest code at or below `db_x10` tenths of a dB, clamped to the range up to
 * and including 0 dB. */
uint8_t es8311_dac_code_from_db_x10(int db_x10);

#ifdef __cplusplus
}
#endif

#endif /* ES8311_CALC_H */
