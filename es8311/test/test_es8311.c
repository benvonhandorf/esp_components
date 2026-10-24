#include <stdio.h>
#include <stdlib.h>

#include "es8311_calc.h"

static int failures;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
            failures++;                                               \
        }                                                             \
    } while (0)

/*
 * Rows from Espressif's published coefficient table (esp-bsp components/es8311,
 * Apache-2.0): {mclk, rate, pre_div, pre_mult}, restricted to rows that use the
 * 256 x fs internal clock this driver is built around. `mult` is the real factor
 * (1, 2, 4, 8); the reference stores its log2 in the register field.
 */
static const struct { uint32_t mclk, rate; uint8_t div, mult; } reference[] = {
    {12288000,  8000, 6, 1}, {16384000,  8000, 8, 1}, { 8192000,  8000, 4, 1},
    { 6144000,  8000, 3, 1}, { 4096000,  8000, 2, 1}, { 2048000,  8000, 1, 1},
    { 1536000,  8000, 3, 4}, { 1024000,  8000, 1, 2},
    {11289600, 11025, 4, 1}, { 5644800, 11025, 2, 1}, { 2822400, 11025, 1, 1},
    { 1411200, 11025, 1, 2},
    {12288000, 12000, 4, 1}, { 6144000, 12000, 2, 1}, { 3072000, 12000, 1, 1},
    { 1536000, 12000, 1, 2},
    {12288000, 16000, 3, 1}, {16384000, 16000, 4, 1}, { 8192000, 16000, 2, 1},
    { 6144000, 16000, 3, 2}, { 4096000, 16000, 1, 1}, { 3072000, 16000, 3, 4},
    { 2048000, 16000, 1, 2}, { 1536000, 16000, 3, 8}, { 1024000, 16000, 1, 4},
    {11289600, 22050, 2, 1}, { 5644800, 22050, 1, 1}, { 2822400, 22050, 1, 2},
    { 1411200, 22050, 1, 4}, {  705600, 22050, 1, 8},
    {12288000, 24000, 2, 1}, { 6144000, 24000, 1, 1}, { 3072000, 24000, 1, 2},
    { 1536000, 24000, 1, 4},
    {12288000, 32000, 3, 2}, {16384000, 32000, 2, 1}, { 8192000, 32000, 1, 1},
    { 6144000, 32000, 3, 4}, { 4096000, 32000, 1, 2}, { 3072000, 32000, 3, 8},
    { 2048000, 32000, 1, 4}, { 1024000, 32000, 1, 8},
    {11289600, 44100, 1, 1}, { 5644800, 44100, 1, 2}, { 2822400, 44100, 1, 4},
    { 1411200, 44100, 1, 8},
    {12288000, 48000, 1, 1}, { 6144000, 48000, 1, 2}, { 3072000, 48000, 1, 4},
    { 1536000, 48000, 1, 8},
};

static void expect_plan(uint32_t clk, uint32_t rate, uint8_t div, uint8_t mult)
{
    es8311_clock_plan_t plan;
    esp_err_t err = es8311_clock_plan(clk, rate, &plan);
    CHECK(err == ESP_OK, "plan %u Hz at %u failed: 0x%x", (unsigned)clk, (unsigned)rate,
          (unsigned)err);
    if (err != ESP_OK) {
        return;
    }
    CHECK(plan.pre_div == div && plan.pre_mult == mult,
          "%u Hz at %u: got /%u x%u, expected /%u x%u", (unsigned)clk, (unsigned)rate,
          plan.pre_div, plan.pre_mult, div, mult);
    CHECK(plan.internal_hz == 256u * rate, "internal clock %u at %u Hz",
          (unsigned)plan.internal_hz, (unsigned)rate);
}

static void expect_unsupported(uint32_t clk, uint32_t rate)
{
    es8311_clock_plan_t plan;
    esp_err_t err = es8311_clock_plan(clk, rate, &plan);
    CHECK(err == ESP_ERR_ES8311_UNSUPPORTED_CLOCK, "%u Hz at %u should be unsupported, got 0x%x",
          (unsigned)clk, (unsigned)rate, (unsigned)err);
}

static void test_reference_rows(void)
{
    for (size_t i = 0; i < sizeof(reference) / sizeof(reference[0]); i++) {
        expect_plan(reference[i].mclk, reference[i].rate, reference[i].div, reference[i].mult);
    }
}

/* No MCLK pin: the bit clock is the input. This is the Cardputer Adv, and the
 * reference table has no rows for it at 8 kHz (256 kHz is not listed). */
static void test_bit_clock_sources(void)
{
    /* 16-bit stereo: BCLK = 32 x fs, needs x8 */
    expect_plan(32u * 8000,  8000,  1, 8);
    expect_plan(32u * 16000, 16000, 1, 8);
    expect_plan(32u * 44100, 44100, 1, 8);
    expect_plan(32u * 48000, 48000, 1, 8);
    /* 24/32-bit stereo in 32-bit slots: BCLK = 64 x fs, needs x4 */
    expect_plan(64u * 8000,  8000,  1, 4);
    expect_plan(64u * 48000, 48000, 1, 4);
}

static void test_unreachable(void)
{
    /* 1.536 MHz is 48 x fs at 32 kHz; 256 needs x5.33 */
    expect_unsupported(1536000, 32000);
    /* 18.432 MHz at 8 kHz would need a /9 */
    expect_unsupported(18432000, 8000);
    /* 3.072 MHz is 384 x fs at 8 kHz; the reference runs it with different
     * ratios, but /3 x2 reaches 256 x fs exactly, so it is supported here. */
    expect_plan(3072000, 8000, 3, 2);

    es8311_clock_plan_t plan;
    CHECK(es8311_clock_plan(2048000, 7999, &plan) == ESP_ERR_INVALID_ARG, "rate below range");
    CHECK(es8311_clock_plan(2048000, 48001, &plan) == ESP_ERR_INVALID_ARG, "rate above range");
    CHECK(es8311_clock_plan(0, 8000, &plan) == ESP_ERR_INVALID_ARG, "zero clock");
    CHECK(es8311_clock_plan(2048000, 8000, NULL) == ESP_ERR_INVALID_ARG, "null out");
}

static void test_register_fields(void)
{
    es8311_clock_plan_t plan;

    es8311_clock_plan(12288000, 8000, &plan);          /* /6 x1 */
    CHECK(es8311_reg02_field(&plan) == 0xA0, "reg02 /6 x1 = 0x%02x", es8311_reg02_field(&plan));
    es8311_clock_plan(256000, 8000, &plan);            /* /1 x8 */
    CHECK(es8311_reg02_field(&plan) == 0x18, "reg02 /1 x8 = 0x%02x", es8311_reg02_field(&plan));
    es8311_clock_plan(1536000, 8000, &plan);           /* /3 x4 */
    CHECK(es8311_reg02_field(&plan) == 0x50, "reg02 /3 x4 = 0x%02x", es8311_reg02_field(&plan));
    es8311_clock_plan(12288000, 48000, &plan);         /* /1 x1 */
    CHECK(es8311_reg02_field(&plan) == 0x00, "reg02 /1 x1 = 0x%02x", es8311_reg02_field(&plan));

    uint8_t f;
    CHECK(es8311_word_length_field(16, &f) && f == 0x0C, "16-bit field");
    CHECK(es8311_word_length_field(18, &f) && f == 0x08, "18-bit field");
    CHECK(es8311_word_length_field(20, &f) && f == 0x04, "20-bit field");
    CHECK(es8311_word_length_field(24, &f) && f == 0x00, "24-bit field");
    CHECK(es8311_word_length_field(32, &f) && f == 0x10, "32-bit field");
    CHECK(!es8311_word_length_field(8, &f), "8-bit must be refused");
}

static void test_volume(void)
{
    CHECK(es8311_dac_code_to_db_x10(0xBF) == 0, "0 dB code");
    CHECK(es8311_dac_code_to_db_x10(0x00) == -955, "bottom code is -95.5 dB");
    CHECK(es8311_dac_code_to_db_x10(0xFF) == 320, "top code is +32 dB");
    CHECK(es8311_dac_code_to_db_x10(0xBE) == -5, "one step is 0.5 dB");

    CHECK(es8311_dac_code_from_db_x10(0) == 0xBF, "0 dB");
    CHECK(es8311_dac_code_from_db_x10(100) == 0xBF, "gain is clamped to 0 dB");
    CHECK(es8311_dac_code_from_db_x10(-1) == 0xBE, "-0.1 dB rounds quieter, not to 0 dB");
    CHECK(es8311_dac_code_from_db_x10(-5) == 0xBE, "-0.5 dB");
    CHECK(es8311_dac_code_from_db_x10(-6) == 0xBD, "-0.6 dB rounds quieter");
    CHECK(es8311_dac_code_from_db_x10(-100) == 0xAB, "-10 dB");
    CHECK(es8311_dac_code_from_db_x10(-955) == 0x00, "bottom");
    CHECK(es8311_dac_code_from_db_x10(-5000) == 0x00, "below the bottom clamps");

    for (int code = 0; code <= ES8311_DAC_CODE_0DB; code++) {
        CHECK(es8311_dac_code_from_db_x10(es8311_dac_code_to_db_x10((uint8_t)code)) == code,
              "round trip of code 0x%02x", code);
    }
}

int main(void)
{
    test_reference_rows();
    test_bit_clock_sources();
    test_unreachable();
    test_register_fields();
    test_volume();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
