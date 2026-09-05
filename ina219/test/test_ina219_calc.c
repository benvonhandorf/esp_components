/*
 * Host test for the INA219's calibration arithmetic, checked against SBOS448G.
 *
 * This one earns its keep: the driver it was extracted from had its calibration
 * register and its assumed current LSB disagree, so every current and power
 * reading it ever produced was about 22% high. Nothing about that was visible
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

/* What the part stores, given what was written. Figure 27: FS0 is void. */
static uint16_t silicon_stores(uint16_t written)
{
    return written & 0xFFFEu;
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

    /*
     * 51 is odd, so the part stored 50: FS0 cannot be written. That makes the
     * real LSB 8.192 mA rather than the 8.03 mA that dividing by 51 suggests,
     * and the real overstatement 22.1% rather than the 24.5% first computed for
     * this bug. See test_fs0_is_void().
     */
    float lsb_at_51 = ina219_current_lsb_for(51, shunt);
    expect(close_to(lsb_at_51, 0.008192f, 1e-6f),
           "CAL=51 is stored as 50, so it yields 8.192 mA/bit, not the 10 mA assumed");

    float overstatement = 0.01f / lsb_at_51;
    expect(close_to(overstatement, 1.2207f, 1e-3f),
           "so the original read about 22.1% high");

    /*
     * And the setting it was reaching for could never have worked either: 10
     * mA/bit across a signed 15-bit register is 327.68 A of full scale, which on
     * a 0.1 ohm shunt is 32.8 V into a 320 mV input.
     */
    ina219_calibration_t cal;
    expect(!ina219_calibration_compute(shunt, 10.0f * 32768.0f / 1000.0f, &cal),
           "a 10 mA/bit range on 0.1 ohm needs 32.8 V of shunt drop, and is refused");
}

static void test_fs0_is_void(void)
{
    /*
     * Figure 27 types FS0 as R-0 where FS15:FS1 are R/W-0, with the note: "FS0
     * is a void bit and will always be 0. It is not possible to write a 1 to
     * FS0. CALIBRATION is the value stored in FS15:FS1."
     *
     * Two things depend on honouring that. configure() decides the part is
     * really present by reading the calibration register back and comparing --
     * the INA219 has no ID registers -- and an odd value never compares equal.
     * And the current LSB has to be derived from what the part stores, not from
     * what was handed to it, or the register and the read path disagree all over
     * again.
     *
     * Roughly half of all otherwise valid configurations land on an odd
     * calibration, so this is not a corner. `written` below is what Equation 1
     * truncates to before the mask -- ordinary shunt and range choices, most of
     * them odd, including the datasheet's own design example.
     */
    static const struct { float shunt; float max_a; uint16_t written; } cases[] = {
        {0.002f, 15.0f, 44739},  /* SBOS448G section 9.2's own design example */
        {0.01f,   8.0f, 16777},
        {0.005f, 20.0f, 13421},
        {0.1f,    3.0f,  4473},
        {0.1f,    1.0f, 13421},
        {0.01f,   5.0f, 26843},
        {0.05f,   6.0f,  4473},
        {0.05f,   5.0f,  5368},  /* already even; the mask is a no-op here */
        {0.1f,    3.2f,  4194},  /* exactly the PGA /8 ceiling, already even */
        {0.002f, 20.0f, 33554},
    };
    unsigned odd_seen = 0;

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char what[128];
        ina219_calibration_t cal;

        snprintf(what, sizeof what, "%g ohm at %g A computes",
                 (double)cases[i].shunt, (double)cases[i].max_a);
        if (!ina219_calibration_compute(cases[i].shunt, cases[i].max_a, &cal)) {
            expect(false, what);
            continue;
        }
        expect(true, what);

        odd_seen += (cases[i].written & 1u);

        snprintf(what, sizeof what,
                 "  Eq.1 gives %u; CAL %u is written, which reads back unchanged",
                 cases[i].written, cal.calibration);
        expect(cal.calibration == silicon_stores(cases[i].written) &&
               cal.calibration == silicon_stores(cal.calibration), what);

        /*
         * The LSB reported must be the one the stored value produces. This is
         * the same check as test_reported_lsb_matches_the_register(), made
         * against the silicon's view rather than the module's.
         */
        float real = 0.04096f / ((float)silicon_stores(cal.calibration) * cases[i].shunt);
        snprintf(what, sizeof what,
                 "  and its LSB (%.7f mA) is the one CAL %u really gives",
                 (double)cal.current_lsb_a * 1e3, silicon_stores(cal.calibration));
        expect(close_to(cal.current_lsb_a, real, 1e-9f), what);
    }

    /* If this ever drops to zero the table has drifted onto even values and
     * stopped exercising the mask at all. */
    expect(odd_seen >= 5, "the table still covers calibrations the part cannot store");

    /* The inverse must model the void bit too, or a value read back off a part
     * is scaled by an LSB the part never used. */
    expect(close_to(ina219_current_lsb_for(4195, 0.1f),
                    ina219_current_lsb_for(4194, 0.1f), 1e-9f),
           "an odd CAL gives the same LSB as the even value the part stores");
    expect(close_to(ina219_current_lsb_for(51, 0.1f),
                    ina219_current_lsb_for(50, 0.1f), 1e-9f),
           "including the original's CAL=51, which the part stored as 50");
    expect(ina219_current_lsb_for(1, 0.1f) == 0.0f,
           "CAL=1 stores as 0, which yields no LSB: the part would read zero forever");
}

static void test_reported_lsb_matches_the_register(void)
{
    /*
     * The fix for the original's bug: report the LSB the calibration register
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

static void test_truncates_rather_than_rounds(void)
{
    /*
     * Equation 1 truncates. Rounding up would shrink the LSB and pull full scale
     * below the range the caller asked for, which is the one direction that
     * silently loses readings.
     */
    ina219_calibration_t cal;
    expect(ina219_calibration_compute(0.1f, 3.0f, &cal), "0.1 ohm at 3 A computes");
    expect(cal.full_scale_a >= 3.0f,
           "full scale covers the requested range rather than falling just under it");

    /* Exact CAL here is 4473.924. Truncation gives 4473 where rounding would
     * have given 4474; FS0 then takes it to 4472. */
    expect(cal.calibration == 4472,
           "CAL is truncated to 4473, not rounded to 4474, then masked to 4472");
}

static void test_refusals(void)
{
    ina219_calibration_t cal;
    expect(!ina219_calibration_compute(0.0f, 10.0f, &cal), "a zero shunt is refused");
    expect(!ina219_calibration_compute(0.1f, 0.0f, &cal), "a zero range is refused");
    expect(!ina219_calibration_compute(0.1f, 10.0f, NULL), "a NULL result is refused");
    expect(ina219_current_lsb_for(0, 0.1f) == 0.0f, "a zero calibration yields no LSB");

    /*
     * The driver programs PGA /8, so it can see 320 mV across the shunt and no
     * more. Past that the front end saturates whatever the calibration says, and
     * reporting a full scale the part cannot reach is worse than refusing.
     */
    expect(!ina219_calibration_compute(0.1f, 10.0f, &cal),
           "10 A across 0.1 ohm is 1 V of shunt drop, past the PGA: refused");
    expect(ina219_calibration_compute(0.1f, 3.2f, &cal),
           "3.2 A across 0.1 ohm is exactly the 320 mV ceiling: accepted");
    expect(!ina219_calibration_compute(0.01f, 50.0f, &cal),
           "50 A across 0.01 ohm is 500 mV: refused");

    /* The opposite end: too little shunt drop overflows the 16-bit register. */
    expect(!ina219_calibration_compute(0.1f, 0.2f, &cal),
           "0.2 A across 0.1 ohm is 20 mV, needing CAL > 65535: refused");
}

static void test_conversions(void)
{
    expect(close_to(ina219_shunt_volts(0x0001), 0.00001f, 1e-9f), "shunt LSB is 10 uV");
    expect(close_to(ina219_shunt_volts(0xFFFF), -0.00001f, 1e-9f), "shunt sign-extends");

    /*
     * Table 7's full-scale codes at each PGA. The register is sign-extended to
     * 16 bits at every gain, so one decoder covers all four.
     */
    expect(close_to(ina219_shunt_volts(0x7D00), 0.320f, 1e-6f), "PGA /8 +FS is 0x7D00");
    expect(close_to(ina219_shunt_volts(0x8300), -0.320f, 1e-6f), "PGA /8 -FS is 0x8300");
    expect(close_to(ina219_shunt_volts(0xF060), -0.040f, 1e-6f), "PGA /1 -FS is 0xF060");

    /* The bus register's value is BD12:BD0; bit 2 is reserved and bits [1:0] are
     * CNVR and OVF: 0x1F98 >> 3 = 0x3F3 = 1011 counts of 4 mV. */
    expect(close_to(ina219_bus_volts(0x1F98), 1011 * 0.004f, 1e-4f),
           "bus voltage ignores the flags in [2:0]");
    expect(close_to(ina219_bus_volts(0x0007), 0.0f, 1e-9f),
           "status bits alone read as zero volts");

    expect(close_to(ina219_current_amps(0xFFFF, 0.01f), -0.01f, 1e-9f),
           "current sign-extends, so reverse current reads negative");
    expect(close_to(ina219_power_watts(100, 0.2f), 20.0f, 1e-4f), "power scales");
    expect(close_to(ina219_power_watts(0xFFFF, 0.02f), 1310.7f, 1e-1f),
           "power is unsigned: 0xFFFF is full scale, not minus one");
}

static void test_datasheet_worked_example(void)
{
    /*
     * SBOS448G section 9.2 and Table 8: a 2 mOhm shunt, 10 A load, 12 V bus,
     * calibrated for 1 mA/bit. Every raw value below is the datasheet's.
     */
    expect(close_to(ina219_shunt_volts(0x07D0), 0.020f, 1e-6f),
           "Table 8: shunt 0x07D0 is 20 mV");
    expect(close_to(ina219_bus_volts(0x5D98), 11.98f, 1e-4f),
           "Table 8: bus 0x5D98 is 11.98 V");
    expect(close_to(ina219_current_amps(0x2710, 0.001f), 10.0f, 1e-4f),
           "Table 8: current 0x2710 at 1 mA/bit is 10.0 A");
    expect(close_to(ina219_power_watts(0x1766, 0.020f), 119.8f, 1e-2f),
           "Table 8: power 0x1766 at 20 mW/bit is 119.8 W");
    expect(close_to(ina219_current_lsb_for(20480, 0.002f), 0.001f, 1e-9f),
           "Table 8: CAL 20480 on a 2 mOhm shunt is 1 mA/bit");
}

int main(void)
{
    test_the_originals_error();
    test_fs0_is_void();
    test_reported_lsb_matches_the_register();
    test_truncates_rather_than_rounds();
    test_refusals();
    test_conversions();
    test_datasheet_worked_example();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
