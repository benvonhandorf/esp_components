/*
 * Host test for the INA226's calibration arithmetic.
 *
 * A wrong calibration does not fail. It produces readings that look entirely
 * reasonable and are wrong by a constant factor -- the hardest measurement bug to
 * notice, and the reason this is worth testing without a part attached.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "ina226_calc.h"

static int failures;

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

static bool close_to(float got, float want, float tolerance)
{
    return fabsf(got - want) <= tolerance;
}

static void test_the_original_board(void)
{
    /*
     * The reference project hardcoded CAL = 512 for a 0.01 ohm shunt at a 1 mA
     * current LSB. Deriving it from the resistor and the full-scale current must
     * reproduce exactly that, or the rework silently changed every reading the
     * original device took.
     *
     * 1 mA/LSB over a signed 15-bit register is 32.768 A full scale.
     */
    ina226_calibration_t cal;
    expect(ina226_calibration_compute(0.01f, 32.768f, &cal), "0.01 ohm at 32.768 A computes");
    expect(cal.calibration == 512, "reproduces the original CAL of 512");
    expect(close_to(cal.current_lsb_a, 0.001f, 1e-9f), "current LSB is 1 mA");
    expect(close_to(cal.power_lsb_w, 0.025f, 1e-9f), "power LSB is 25x the current LSB");
    expect(close_to(cal.full_scale_a, 32.768f, 1e-4f), "full scale is as asked");
}

static void test_other_shunts(void)
{
    ina226_calibration_t cal;

    /* A 0.1 ohm shunt at 3.2768 A: current LSB 100 uA, CAL = 0.00512/(1e-4*0.1) = 512. */
    expect(ina226_calibration_compute(0.1f, 3.2768f, &cal), "0.1 ohm computes");
    expect(cal.calibration == 512, "a tenfold shunt with a tenfold smaller range gives the same CAL");
    expect(close_to(cal.current_lsb_a, 0.0001f, 1e-9f), "and a tenfold smaller LSB");

    /* 0.002 ohm at 15 A -- a realistic high-current design. */
    expect(ina226_calibration_compute(0.002f, 15.0f, &cal), "0.002 ohm at 15 A computes");
    expect(cal.calibration > 0, "produces a usable calibration");
    /* CAL = 0.00512 / ((15/32768) * 0.002) = 5592.4 -> 5592 */
    expect(cal.calibration == 5592, "matches the datasheet formula, rounded");
}

static void test_rounding_is_not_truncation(void)
{
    /*
     * Truncating biases every reading low by up to one LSB of calibration, which
     * is a systematic scale error rather than noise. Pick values whose exact CAL
     * lands just above a .5 boundary.
     */
    ina226_calibration_t cal;
    expect(ina226_calibration_compute(0.002f, 15.0f, &cal), "computes");
    /* 5592.405... rounds to 5592; truncation gives the same here, so use a case
     * where they differ: CAL = 0.00512/((10/32768)*0.003) = 5592.4 -> check a
     * fractional case explicitly. */
    expect(ina226_calibration_compute(0.0033f, 12.0f, &cal), "computes for 0.0033 ohm");
    /* 0.00512 / ((12/32768) * 0.0033) = 4236.7... -> 4237 rounded, 4236 truncated. */
    expect(cal.calibration == 4237, "rounds up rather than truncating");
}

static void test_refusals(void)
{
    ina226_calibration_t cal;
    expect(!ina226_calibration_compute(0.0f, 10.0f, &cal), "a zero shunt is refused");
    expect(!ina226_calibration_compute(-0.01f, 10.0f, &cal), "a negative shunt is refused");
    expect(!ina226_calibration_compute(0.01f, 0.0f, &cal), "a zero full scale is refused");
    expect(!ina226_calibration_compute(0.01f, 10.0f, NULL), "a NULL result is refused");

    /* A tiny shunt with a tiny range overflows the 16-bit register; wrapping
     * would give a silently wrong scale. */
    expect(!ina226_calibration_compute(0.0001f, 0.1f, &cal),
           "a calibration that will not fit 16 bits is refused");
}

static void test_conversions(void)
{
    /* Shunt and current are two's complement: a discharge reads negative. */
    expect(close_to(ina226_shunt_volts(0x0001), 0.0000025f, 1e-9f), "shunt LSB is 2.5 uV");
    expect(close_to(ina226_shunt_volts(0xFFFF), -0.0000025f, 1e-9f),
           "shunt sign-extends, so reverse current reads negative");
    expect(close_to(ina226_current_amps(0xFFFF, 0.001f), -0.001f, 1e-9f),
           "current sign-extends");
    expect(close_to(ina226_current_amps(0x0064, 0.001f), 0.1f, 1e-6f), "current scales");

    /*
     * The bus register is unsigned. The original cast it to int16_t, which happens
     * to work only because the part's 36 V ceiling lands below 0x7FFF -- at
     * 0x8000 a signed read would report -40.96 V for 40.96 V.
     */
    expect(close_to(ina226_bus_volts(0x0001), 0.00125f, 1e-9f), "bus LSB is 1.25 mV");
    expect(ina226_bus_volts(0x8000) > 0.0f,
           "a bus reading above 0x7FFF stays positive");
    expect(close_to(ina226_bus_volts(0x8000), 40.96f, 1e-3f), "and reads full scale");

    expect(close_to(ina226_power_watts(100, 0.025f), 2.5f, 1e-6f), "power scales");
}

int main(void)
{
    test_the_original_board();
    test_other_shunts();
    test_rounding_is_not_truncation();
    test_refusals();
    test_conversions();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
