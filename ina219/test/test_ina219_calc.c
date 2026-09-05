/*
 * Host test for the INA219's calibration arithmetic.
 *
 * This one earns its keep: the driver it was extracted from had its calibration
 * register and its assumed current LSB disagree, so every current and power
 * reading it ever produced was about 24.5% high. Nothing about that was visible
 * without doing the arithmetic, which is what this file does.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "ina219_calc.h"

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

static void test_the_originals_error(void)
{
    /*
     * The original wrote CAL = 51 for a 0.1 ohm shunt and then scaled every
     * current reading by 10 mA/bit. Those disagree.
     *
     * Its comment read: "R100 shunt (0.1R) with 10mA LSB: CAL = 0.00512 /
     * (0.01 * 0.1) = 51.2". Two things are wrong with that line. 0.00512 is the
     * INA226's scaling constant -- the INA219's is 0.04096 -- and the arithmetic
     * as written gives 5.12, not 51.2.
     */
    const float shunt = 0.1f;

    float lsb_at_51 = ina219_current_lsb_for(51, shunt);
    expect(close_to(lsb_at_51, 0.008031f, 1e-5f),
           "CAL=51 on a 0.1 ohm shunt yields 8.03 mA/bit, not the 10 mA assumed");

    float overstatement = 0.01f / lsb_at_51;
    expect(close_to(overstatement, 1.2451f, 1e-3f),
           "so the original read about 24.5% high");

    /* What CAL should have been for a true 10 mA/bit. */
    ina219_calibration_t cal;
    expect(ina219_calibration_compute(shunt, 10.0f * 32768.0f / 1000.0f, &cal),
           "a 10 mA/bit range computes");
    expect(cal.calibration == 41,
           "CAL for 10 mA/bit on 0.1 ohm is 41, not 51");
    expect(close_to(cal.current_lsb_a, 0.00999f, 1e-4f),
           "and that calibration really does give ~10 mA/bit");
}

static void test_reported_lsb_matches_the_register(void)
{
    /*
     * The fix for the original's bug: report the LSB the rounded register
     * actually produces, not the one that was requested. A caller scaling by the
     * requested value reintroduces exactly the same error.
     */
    ina219_calibration_t cal;
    expect(ina219_calibration_compute(0.1f, 3.2f, &cal), "computes");

    float from_register = ina219_current_lsb_for(cal.calibration, 0.1f);
    expect(close_to(cal.current_lsb_a, from_register, 1e-9f),
           "the reported LSB is derived from the register, not from the request");
    expect(close_to(cal.power_lsb_w, cal.current_lsb_a * 20.0f, 1e-9f),
           "power LSB is 20x the current LSB (the INA226's is 25x)");
}

static void test_refusals(void)
{
    ina219_calibration_t cal;
    expect(!ina219_calibration_compute(0.0f, 10.0f, &cal), "a zero shunt is refused");
    expect(!ina219_calibration_compute(0.1f, 0.0f, &cal), "a zero range is refused");
    expect(!ina219_calibration_compute(0.1f, 10.0f, NULL), "a NULL result is refused");
    expect(ina219_current_lsb_for(0, 0.1f) == 0.0f, "a zero calibration yields no LSB");
}

static void test_conversions(void)
{
    expect(close_to(ina219_shunt_volts(0x0001), 0.00001f, 1e-9f), "shunt LSB is 10 uV");
    expect(close_to(ina219_shunt_volts(0xFFFF), -0.00001f, 1e-9f), "shunt sign-extends");

    /* The bus register's low three bits are CNVR and OVF, not part of the value:
     * 0x1F98 >> 3 = 0x3F3 = 1011 counts of 4 mV. */
    expect(close_to(ina219_bus_volts(0x1F98), 1011 * 0.004f, 1e-4f),
           "bus voltage ignores the status bits in [2:0]");
    expect(close_to(ina219_bus_volts(0x0007), 0.0f, 1e-9f),
           "status bits alone read as zero volts");

    expect(close_to(ina219_current_amps(0xFFFF, 0.01f), -0.01f, 1e-9f),
           "current sign-extends, so reverse current reads negative");
    expect(close_to(ina219_power_watts(100, 0.2f), 20.0f, 1e-4f), "power scales");
}

int main(void)
{
    test_the_originals_error();
    test_reported_lsb_matches_the_register();
    test_refusals();
    test_conversions();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
