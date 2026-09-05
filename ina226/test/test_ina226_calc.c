/*
 * Host test for the INA226's calibration arithmetic and part identification,
 * checked against SBOS547C.
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
     * current LSB -- 32.768 A of full scale over a signed 15-bit register.
     *
     * That configuration is not physically reachable. 32.768 A across 0.01 ohm
     * develops 328 mV, and the INA226's shunt input stops at 81.92 mV with no
     * PGA to widen it, so everything above 8.192 A read as 8.192 A. The old
     * driver accepted it and reported 32.768 A of range regardless.
     */
    ina226_calibration_t cal;
    expect(!ina226_calibration_compute(0.01f, 32.768f, &cal),
           "the original 0.01 ohm / 32.768 A setting needs 328 mV, and is refused");

    /*
     * What that board should have asked for. The shunt fixes the ceiling, and
     * using all of it also buys back the two bits the original threw away: 250
     * uA/bit rather than 1 mA/bit.
     */
    expect(ina226_calibration_compute(0.01f, 8.192f, &cal),
           "0.01 ohm at 8.192 A -- the ceiling that shunt allows -- computes");
    expect(cal.calibration == 2048, "CAL is 2048, the smallest the input range allows");
    expect(close_to(cal.current_lsb_a, 0.00025f, 1e-9f), "current LSB is 250 uA, not 1 mA");
    expect(close_to(cal.full_scale_a, 8.192f, 1e-3f), "and full scale is the real 8.192 A");
}

static void test_reported_lsb_matches_the_register(void)
{
    /*
     * The LSB reported has to be the one the calibration register produces, not
     * the one that was requested. They differ whenever the calibration does not
     * come out a whole number, and a caller scaling by the requested value puts
     * that difference into every current and power reading -- which is exactly
     * the bug the sibling INA219 driver was written to fix.
     */
    static const struct { float shunt; float max_a; } cases[] = {
        {0.002f,  15.0f},   /* the datasheet's shunt, at the minimum LSB */
        {0.0033f, 12.0f},
        {0.01f,    5.0f},
        {0.05f,    1.5f},
        {0.1f,     0.6f},
    };

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char what[128];
        ina226_calibration_t cal;

        snprintf(what, sizeof what, "%g ohm at %g A computes",
                 (double)cases[i].shunt, (double)cases[i].max_a);
        if (!ina226_calibration_compute(cases[i].shunt, cases[i].max_a, &cal)) {
            expect(false, what);
            continue;
        }
        expect(true, what);

        float from_register = ina226_current_lsb_for(cal.calibration, cases[i].shunt);
        snprintf(what, sizeof what,
                 "  CAL %u yields %.9f A/bit, and that is what is reported",
                 cal.calibration, (double)from_register);
        expect(close_to(cal.current_lsb_a, from_register, 1e-12f), what);

        /* The requested LSB is what the old code reported. Where the two differ,
         * that difference was a scale error in every reading it produced. */
        float requested = cases[i].max_a / 32768.0f;
        printf("      (the requested %.9f A/bit differs by %+.4f%%)\n",
               (double)requested,
               (double)((requested - from_register) / from_register * 100.0f));

        expect(close_to(cal.power_lsb_w, cal.current_lsb_a * 25.0f, 1e-9f),
               "  power LSB is 25x the current LSB");
    }
}

static void test_calibration_register_is_fifteen_bits(void)
{
    /*
     * Table 7-11 names the calibration field FS14:FS0 and leaves D15 unnamed, so
     * the largest value the part can hold is 32767 -- not the 65535 a 16-bit
     * register would suggest.
     *
     * CAL is 0.00512 x 2^15 divided by the full-scale shunt drop, so the limit
     * bites below about 5.12 mV of drop. Accepting a larger value would drop the
     * top bit and rescale every reading by more than a factor of two.
     */
    ina226_calibration_t cal;

    expect(ina226_calibration_compute(0.1f, 0.0514f, &cal),
           "0.1 ohm at 51.4 mA (5.14 mV) is inside the register");
    expect(cal.calibration <= INA226_CAL_MAX,
           "  and its CAL fits fifteen bits");

    expect(!ina226_calibration_compute(0.1f, 0.0512f, &cal),
           "0.1 ohm at 51.2 mA needs CAL 32768 -- one past the register -- and is refused");
    expect(!ina226_calibration_compute(0.1f, 0.03f, &cal),
           "0.1 ohm at 30 mA needs CAL 55924, which would truncate to 23156 and "
           "rescale readings by 2.4x: refused");
    expect(!ina226_calibration_compute(0.01f, 0.3f, &cal),
           "0.01 ohm at 300 mA is the same 3 mV drop, and equally refused");
}

static void test_shunt_range_ceiling(void)
{
    /*
     * The shunt input is fixed at +/-81.92 mV. Unlike the INA219 there is no PGA,
     * so a range needing more drop than that cannot be measured at all and
     * reporting it as full scale is a lie the caller acts on.
     */
    ina226_calibration_t cal;

    expect(ina226_calibration_compute(0.01f, 8.192f, &cal),
           "0.01 ohm at 8.192 A is exactly 81.92 mV: accepted");
    expect(ina226_calibration_compute(0.1f, 0.8192f, &cal),
           "0.1 ohm at 819.2 mA is exactly 81.92 mV: accepted");
    expect(close_to(cal.current_lsb_a, 0.000025f, 1e-9f), "  at 25 uA/bit");

    expect(!ina226_calibration_compute(0.1f, 3.2768f, &cal),
           "0.1 ohm at 3.2768 A is 328 mV: refused");
    expect(!ina226_calibration_compute(0.002f, 50.0f, &cal),
           "0.002 ohm at 50 A is 100 mV: refused");
    expect(ina226_calibration_compute(0.002f, 40.0f, &cal),
           "0.002 ohm at 40 A is 80 mV: accepted");
}

static void test_die_id_ignores_the_revision(void)
{
    /*
     * Table 7-15 splits the die ID into DID15:4 and RID3:0, the die revision.
     * The register map lists both 2260h and 2261h for this part -- "Die COO:
     * 2260 = USA or Japan, 2261 = USA" -- so comparing all sixteen bits rejects
     * genuine INA226s off the wrong line.
     */
    expect(ina226_part_matches(0x5449, 0x2260), "die 2260h is an INA226");
    expect(ina226_part_matches(0x5449, 0x2261), "so is die 2261h, a different die revision");
    expect(ina226_part_matches(0x5449, 0x226F), "so is any other revision of the same device");

    expect(!ina226_part_matches(0x5449, 0x2270), "a different device ID is not");
    expect(!ina226_part_matches(0x5449, 0x2250), "nor is 225xh");
    expect(!ina226_part_matches(0x5449, 0x0000), "nor is a part that answers with zeros");
    expect(!ina226_part_matches(0x1234, 0x2260), "and the manufacturer still has to be TI");
    expect(!ina226_part_matches(0x0000, 0x2260), "including when it answers with zeros");
}

static void test_refusals(void)
{
    ina226_calibration_t cal;
    expect(!ina226_calibration_compute(0.0f, 10.0f, &cal), "a zero shunt is refused");
    expect(!ina226_calibration_compute(-0.01f, 10.0f, &cal), "a negative shunt is refused");
    expect(!ina226_calibration_compute(0.01f, 0.0f, &cal), "a zero full scale is refused");
    expect(!ina226_calibration_compute(0.01f, 10.0f, NULL), "a NULL result is refused");
    expect(!ina226_calibration_compute(0.0001f, 0.1f, &cal),
           "a tiny shunt with a tiny range overflows the calibration and is refused");
    expect(ina226_current_lsb_for(0, 0.01f) == 0.0f, "a zero calibration yields no LSB");
    expect(ina226_current_lsb_for(2048, 0.0f) == 0.0f, "nor does a zero shunt");
}

static void test_conversions(void)
{
    expect(close_to(ina226_shunt_volts(0x0001), 0.0000025f, 1e-9f), "shunt LSB is 2.5 uV");
    expect(close_to(ina226_shunt_volts(0xFFFF), -0.0000025f, 1e-9f),
           "shunt sign-extends, so reverse current reads negative");
    /* Table 7-7: full scale is 81.92 mV at 7FFFh. */
    expect(close_to(ina226_shunt_volts(0x7FFF), 0.0819175f, 1e-6f),
           "0x7FFF is 81.9175 mV, the positive full scale");
    expect(close_to(ina226_shunt_volts(0x8000), -0.08192f, 1e-6f),
           "0x8000 is -81.92 mV, the negative full scale");

    expect(close_to(ina226_current_amps(0xFFFF, 0.001f), -0.001f, 1e-9f),
           "current sign-extends");
    expect(close_to(ina226_current_amps(0x0064, 0.001f), 0.1f, 1e-6f), "current scales");

    expect(close_to(ina226_bus_volts(0x0001), 0.00125f, 1e-9f), "bus LSB is 1.25 mV");
    expect(ina226_bus_volts(0x8000) > 0.0f,
           "the bus register is unsigned: 0x8000 is a positive voltage");
    expect(close_to(ina226_bus_volts(0x8000), 40.96f, 1e-3f), "and reads full scale");

    expect(close_to(ina226_power_watts(100, 0.025f), 2.5f, 1e-6f), "power scales");
    expect(close_to(ina226_power_watts(0xFFFF, 0.025f), 1638.375f, 1e-2f),
           "power is unsigned: 0xFFFF is full scale, not minus one");
}

static void test_datasheet_worked_example(void)
{
    /*
     * SBOS547C Table 6-1: a 2 mOhm shunt, 10 A load, 12 V bus, calibrated for
     * 1 mA/bit. Every raw value below is the datasheet's.
     */
    ina226_calibration_t cal;
    expect(ina226_calibration_compute(0.002f, 32.768f, &cal),
           "Table 6-1: 2 mOhm at 1 mA/bit computes");
    expect(cal.calibration == 2560, "  Eq.1 gives CAL 2560 (0xA00), as step 4 says");
    expect(close_to(cal.current_lsb_a, 0.001f, 1e-9f), "  at exactly 1 mA/bit");
    expect(close_to(cal.power_lsb_w, 0.025f, 1e-7f), "  and 25 mW/bit");

    expect(close_to(ina226_shunt_volts(0x1F40), 0.020f, 1e-6f),
           "  step 2: shunt 0x1F40 is 20 mV");
    expect(close_to(ina226_bus_volts(0x2570), 11.98f, 1e-4f),
           "  step 3: bus 0x2570 is 11.98 V");
    expect(close_to(ina226_current_amps(0x2710, 0.001f), 10.0f, 1e-4f),
           "  step 5: current 0x2710 at 1 mA/bit is 10 A");
    expect(close_to(ina226_power_watts(0x12B8, 0.025f), 119.8f, 1e-2f),
           "  step 6: power 0x12B8 at 25 mW/bit is 119.8 W");

    /* Eq. 3 and Eq. 4, which the part computes internally, close the loop. */
    expect((8000 * 2560) / 2048 == 10000,
           "  Eq.3: shunt 8000 x CAL 2560 / 2048 = current 10000");
    expect((10000 * 9584) / 20000 == 4792,
           "  Eq.4: current 10000 x bus 9584 / 20000 = power 4792");
}

int main(void)
{
    test_the_original_board();
    test_reported_lsb_matches_the_register();
    test_calibration_register_is_fifteen_bits();
    test_shunt_range_ceiling();
    test_die_id_ignores_the_revision();
    test_refusals();
    test_conversions();
    test_datasheet_worked_example();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
