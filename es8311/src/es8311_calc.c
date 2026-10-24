#include "es8311_calc.h"

esp_err_t es8311_clock_plan(uint32_t clk_in_hz, uint32_t rate_hz,
                            es8311_clock_plan_t *out)
{
    if (!out || rate_hz < ES8311_RATE_MIN_HZ || rate_hz > ES8311_RATE_MAX_HZ ||
        clk_in_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t target = (uint64_t)ES8311_MCLK_MULTIPLE * rate_hz;

    /* Smallest divider first: with a 12.288 MHz MCLK at 8 kHz, /6 and /12 both
     * reach the target and the first is the one to use. */
    for (uint8_t div = 1; div <= 8; div++) {
        for (uint8_t mult = 1; mult <= 8; mult <<= 1) {
            if ((uint64_t)clk_in_hz * mult == target * div) {
                out->pre_div = div;
                out->pre_mult = mult;
                out->internal_hz = (uint32_t)target;
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_ES8311_UNSUPPORTED_CLOCK;
}

uint8_t es8311_reg02_field(const es8311_clock_plan_t *plan)
{
    uint8_t mult_code = 0;
    for (uint8_t m = plan->pre_mult; m > 1; m >>= 1) {
        mult_code++;
    }
    return (uint8_t)(((plan->pre_div - 1) << 5) | (mult_code << 3));
}

bool es8311_word_length_field(uint8_t bits, uint8_t *field)
{
    uint8_t f;
    switch (bits) {
        case 16: f = 3; break;
        case 18: f = 2; break;
        case 20: f = 1; break;
        case 24: f = 0; break;
        case 32: f = 4; break;
        default: return false;
    }
    *field = (uint8_t)(f << 2);
    return true;
}

int es8311_dac_code_to_db_x10(uint8_t code)
{
    return ((int)code - ES8311_DAC_CODE_0DB) * 5;
}

uint8_t es8311_dac_code_from_db_x10(int db_x10)
{
    if (db_x10 >= 0) {
        return ES8311_DAC_CODE_0DB;
    }
    /* Round toward quieter: a request for -0.3 dB must not come back as 0 dB. */
    int steps_down = (-db_x10 + 4) / 5;
    int code = ES8311_DAC_CODE_0DB - steps_down;
    return (uint8_t)(code < ES8311_DAC_CODE_MIN ? ES8311_DAC_CODE_MIN : code);
}
