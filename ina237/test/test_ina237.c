/*
 * Host test for the INA237, checked against SBOSA20A.
 *
 * This driver's arithmetic validated clean against the datasheet, so these are
 * regression tests: they pin the identity that makes a single constant
 * SHUNT_CAL correct for every shunt, and the decode path that turns raw
 * registers into volts, amps, watts and degrees.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "fake_i2c.h"
#include "ina237.h"

static int failures;

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

static bool close_to(double got, double want, double tolerance)
{
    return fabs(got - want) <= tolerance;
}

/* Equation 1, written out independently of the driver. */
static double shunt_cal_eq1(double current_lsb, double shunt_ohms)
{
    return 819.2e6 * current_lsb * shunt_ohms;
}

static ina237_handle_t create_on_fake(void)
{
    const ina237_config_t cfg = {
        .dev = (i2c_master_dev_handle_t)&fake,
        .shunt_ohms = 0.0,   /* configure() is what fixes the shunt */
    };
    ina237_handle_t h = NULL;
    return ina237_create(&cfg, &h) == ESP_OK ? h : NULL;
}

static ina237_handle_t make(double shunt_ohms)
{
    fake_reset();
    ina237_handle_t h = create_on_fake();
    if (!h) {
        return NULL;
    }
    if (ina237_configure(h, shunt_ohms, NULL) != ESP_OK) {
        ina237_delete(h);
        return NULL;
    }
    return h;
}

static void test_shunt_cal_is_one_constant_for_every_shunt(void)
{
    /*
     * Equation 1 is SHUNT_CAL = 819.2e6 x CURRENT_LSB x RSHUNT, which normally
     * makes the register depend on the sense resistor. This driver picks
     * CURRENT_LSB so the current register spans exactly the ADC's own range --
     * 163.84 mV / RSHUNT over 2^15 counts, which is the 5 uV shunt LSB divided
     * by the resistance -- and substituting that back cancels RSHUNT entirely.
     *
     * If that identity ever stops holding, a fixed SHUNT_CAL becomes wrong for
     * every shunt but one, so it is worth pinning across the whole range.
     */
    static const double shunts[] = {
        0.0005, 0.001, 0.002, 0.005, 0.01, 0.016384, 0.02, 0.05, 0.1, 1.0,
    };

    for (unsigned i = 0; i < sizeof shunts / sizeof *shunts; i++) {
        char what[144];
        const double r = shunts[i];
        const double lsb = ina237_current_lsb_for(r);

        snprintf(what, sizeof what,
                 "R=%-8g CURRENT_LSB %.6e A -> Eq.1 SHUNT_CAL %.4f",
                 r, lsb, shunt_cal_eq1(lsb, r));
        expect(close_to(shunt_cal_eq1(lsb, r), INA237_SHUNT_CAL_VALUE, 1e-6), what);

        /* The same LSB has to be the full-scale span over 2^15, or the current
         * register and the ADC disagree about what full scale means. */
        snprintf(what, sizeof what,
                 "  and equals full scale (%.4f A) / 2^15", INA237_SHUNT_FULL_SCALE_V / r);
        expect(close_to(lsb, (INA237_SHUNT_FULL_SCALE_V / r) / 32768.0, 1e-18), what);
    }

    expect(INA237_SHUNT_CAL_VALUE == 4096,
           "819.2e6 x 5 uV = 4096, so the constant is exact, not rounded");
    expect(close_to(INA237_SHUNT_FULL_SCALE_V / 5.0e-6, 32768.0, 1e-9),
           "163.84 mV over a 5 uV LSB is exactly 2^15 counts");
    expect(ina237_current_lsb_for(0.0) == 0.0, "a zero shunt yields no LSB");
    expect(ina237_current_lsb_for(-1.0) == 0.0, "nor does a negative one");
}

static void test_datasheet_design_example(void)
{
    /*
     * SBOSA20A section 8.2.2.3: a maximum current of 10 A gives CURRENT_LSB
     * 305.1758 uA, and Equation 1 with the example's 16.2 mOhm shunt gives
     * SHUNT_CAL 4050 (FD2h).
     */
    const double lsb = 10.0 / 32768.0;
    expect(close_to(lsb * 1e6, 305.1758, 1e-4),
           "Eq.2: 10 A / 2^15 is 305.1758 uA, as the datasheet says");
    expect(close_to(shunt_cal_eq1(lsb, 0.0162), 4050.0, 0.05),
           "Eq.1 with a 16.2 mOhm shunt gives SHUNT_CAL 4050 (0xFD2)");

    /* And the driver's own choice for that shunt: use all of the ADC range. */
    expect(close_to(ina237_current_lsb_for(0.016384), lsb, 1e-12),
           "a 16.384 mOhm shunt puts full scale at exactly 10 A, at the same LSB");
}

static void test_configure_identifies_and_calibrates(void)
{
    ina237_handle_t h = NULL;
    ina237_config_report_t report;

    fake_reset();
    h = create_on_fake();
    expect(h != NULL, "create succeeds");
    expect(ina237_configure(h, 0.01, &report) == ESP_OK, "configure succeeds against a TI part");
    expect(report.manufacturer_id == 0x5449, "  and reports the manufacturer it read");
    expect(fake.reg[0x02] == INA237_SHUNT_CAL_VALUE, "  SHUNT_CAL was written with 4096");
    expect(close_to(report.current_lsb, 500e-6, 1e-12),
           "  0.01 ohm gives a 500 uA/bit current LSB");
    expect(close_to(report.full_scale_amps, 16.384, 1e-9),
           "  and 16.384 A of full scale, which is the ADC range over the shunt");
    ina237_delete(h);

    /* A part that answers with something other than "TI". */
    fake_reset();
    fake.reg[0x3E] = 0x2260;   /* an INA226's die ID, say */
    h = create_on_fake();
    expect(h != NULL, "create succeeds again");
    expect(ina237_configure(h, 0.01, &report) == ESP_ERR_INA237_WRONG_PART,
           "a part that is not TI's is refused");
    expect(report.failed_stage == INA237_STAGE_IDENTIFY, "  and the stage says why");
    expect(report.manufacturer_id == 0x2260, "  reporting what actually answered");
    ina237_delete(h);

    /* Nothing on the bus at all. */
    fake_reset();
    fake.present = false;
    h = create_on_fake();
    expect(h != NULL, "create still succeeds");
    expect(ina237_configure(h, 0.01, &report) != ESP_OK, "configure fails with no part present");
    expect(report.failed_stage == INA237_STAGE_PROBE, "  at the probe stage");
    ina237_delete(h);
}

static void test_decode(void)
{
    /*
     * Table 7-9 through 7-12: VBUS unsigned at 3.125 mV, VSHUNT two's complement
     * at 5 uV (ADCRANGE = 0), DIETEMP a signed 12-bit value in bits 15:4 at
     * 125 m degrees, CURRENT two's complement, POWER 24-bit unsigned.
     */
    ina237_handle_t h = make(0.01);      /* CURRENT_LSB 500 uA/bit */
    ina237_reading_t r;
    expect(h != NULL, "a 0.01 ohm part configures");

    fake.reg[0x05] = 3840;               /* VBUS: 3840 x 3.125 mV = 12 V */
    fake.reg[0x04] = 4000;               /* VSHUNT: 4000 x 5 uV = 20 mV */
    fake.reg[0x06] = 0x0C80;             /* DIETEMP: 0x0C8 = 200 -> 25 C */
    fake.reg[0x07] = 4000;               /* CURRENT: 4000 x 500 uA = 2 A */
    fake.reg[0x08] = 24000;              /* POWER low 16 */
    fake.power_high = 0;                 /* 24000 x 0.2 x 500 uA = 2.4 W */

    expect(ina237_read(h, &r) == ESP_OK, "read succeeds");
    expect(close_to(r.bus_v, 12.0, 1e-9), "  bus is 12 V");
    expect(close_to(r.shunt_v, 0.020, 1e-9), "  shunt is 20 mV");
    expect(close_to(r.temp_c, 25.0, 1e-9), "  die temperature is 25 C");
    expect(close_to(r.current_a, 2.0, 1e-9), "  current is 2 A");
    expect(close_to(r.power_w, 2.4, 1e-9), "  power is 2.4 W (Eq.4: 0.2 x LSB x POWER)");
    expect(r.trim_checksum_ok, "  MEMSTAT says the trim checksum is good");
    expect(!r.math_overflow, "  and MATHOF is clear");
    expect(r.shunt_cal_matches, "  SHUNT_CAL still reads back what was written");

    /* Negative shunt voltage and current: two's complement, sign-extended. */
    fake.reg[0x04] = (uint16_t)(int16_t)-4000;
    fake.reg[0x07] = (uint16_t)(int16_t)-4000;
    fake.reg[0x06] = 0xFF60;             /* -160 >> 4 = -10 -> -1.25 C */
    expect(ina237_read(h, &r) == ESP_OK, "read succeeds with reverse current");
    expect(close_to(r.shunt_v, -0.020, 1e-9), "  shunt is -20 mV");
    expect(close_to(r.current_a, -2.0, 1e-9), "  current is -2 A");
    expect(close_to(r.temp_c, -1.25, 1e-9), "  and a sub-zero die temperature decodes");

    /* DIETEMP's low nibble is reserved and must not reach the result. */
    fake.reg[0x06] = 0x0C8F;
    expect(ina237_read(h, &r) == ESP_OK, "read succeeds");
    expect(close_to(r.temp_c, 25.0, 1e-9),
           "  the reserved low nibble of DIETEMP is ignored");

    /* The POWER register is 24 bits, so the top byte has to be picked up. */
    fake.reg[0x08] = 0x0000;
    fake.power_high = 0x01;              /* 65536 x 0.2 x 500 uA = 6.5536 W */
    expect(ina237_read(h, &r) == ESP_OK, "read succeeds");
    expect(close_to(r.power_w, 6.5536, 1e-9),
           "  POWER is assembled from all 24 bits, not just the low 16");

    /* Flags the caller is meant to act on. */
    fake.reg[0x0B] = 0x0200;             /* MATHOF set, MEMSTAT clear */
    fake.reg[0x02] = 0x0000;             /* as if the part had been reset */
    expect(ina237_read(h, &r) == ESP_OK, "read succeeds");
    expect(r.math_overflow, "  MATHOF is reported");
    expect(!r.trim_checksum_ok, "  a clear MEMSTAT is reported as a bad checksum");
    expect(!r.shunt_cal_matches, "  and a lost SHUNT_CAL is noticed");

    ina237_delete(h);
}

int main(void)
{
    test_shunt_cal_is_one_constant_for_every_shunt();
    test_datasheet_design_example();
    test_configure_identifies_and_calibrates();
    test_decode();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
