/* Host test for the LM75BDP's temperature encodings. Two two's-complement
 * formats, neither byte-aligned -- the kind of thing that reads correctly above
 * freezing and wrongly below it. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "lm75bdp_calc.h"

static int failures;
static void expect(bool c, const char *w)
{ if (!c) { failures++; printf("FAIL: %s\n", w); } else printf("ok  : %s\n", w); }
static bool close_to(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_temperature(void)
{
    expect(close_to(lm75bdp_temperature_c(0x0000), 0.0f), "zero reads 0 C");
    /* 25.0 C = 200 steps of 0.125, left-justified by 5. */
    expect(close_to(lm75bdp_temperature_c(200 << 5), 25.0f), "25 C reads back");
    expect(close_to(lm75bdp_temperature_c(1 << 5), 0.125f), "one LSB is 0.125 C");
    /* -25.0 C = -200 steps. */
    /* Shift the magnitude, then negate: shifting a negative value left is
     * undefined behaviour, which -Werror rightly refuses. */
    expect(close_to(lm75bdp_temperature_c((uint16_t)(-(200 << 5))), -25.0f),
           "sub-zero reads negative");
    expect(lm75bdp_temperature_c(0xFFE0) < 0.0f, "the all-ones pattern is negative");
    /* The low five bits are undefined and must not shift the value. */
    expect(close_to(lm75bdp_temperature_c((200 << 5) | 0x1F), 25.0f),
           "the undefined low bits are ignored");
}

static void test_thresholds_round_trip(void)
{
    const float values[] = {0.0f, 0.5f, 25.0f, 80.0f, -0.5f, -10.0f, -55.0f, 127.5f};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        float back = lm75bdp_decode_threshold(lm75bdp_encode_threshold(values[i]));
        char label[64];
        snprintf(label, sizeof(label), "%.1f C survives a threshold round trip", (double)values[i]);
        expect(close_to(back, values[i]), label);
    }
}

static void test_threshold_rounding(void)
{
    /*
     * The original truncated, which moves toward zero: -0.7 C became -0.5,
     * putting the threshold on the wrong side of the temperature it was meant to
     * catch -- and only below freezing, so a bench test at room temperature
     * would never show it.
     */
    expect(close_to(lm75bdp_decode_threshold(lm75bdp_encode_threshold(-0.7f)), -0.5f),
           "-0.7 C rounds to the nearer half-degree (-0.5)");
    expect(close_to(lm75bdp_decode_threshold(lm75bdp_encode_threshold(-0.8f)), -1.0f),
           "-0.8 C rounds away from zero, which truncation would not");
    expect(close_to(lm75bdp_decode_threshold(lm75bdp_encode_threshold(0.8f)), 1.0f),
           "0.8 C rounds up");
}

static void test_threshold_clamping(void)
{
    /* Masking a value past the 9-bit range would wrap a hot limit into a cold
     * one, so it is clamped instead. */
    expect(close_to(lm75bdp_decode_threshold(lm75bdp_encode_threshold(200.0f)), 127.5f),
           "an over-range hot threshold clamps rather than wrapping");
    expect(close_to(lm75bdp_decode_threshold(lm75bdp_encode_threshold(-200.0f)), -128.0f),
           "an over-range cold threshold clamps");
}

int main(void)
{
    test_temperature();
    test_thresholds_round_trip();
    test_threshold_rounding();
    test_threshold_clamping();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
