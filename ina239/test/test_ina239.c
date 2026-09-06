/*
 * Host test for the INA239, checked against SLYS027A.
 *
 * Two things are worth pinning here and neither is observable on hardware
 * without a bench supply and a known load. The first is the SPI frame: a
 * command byte with the address in the wrong bits addresses a real register,
 * just not the intended one, and reads back a plausible number. The second is
 * the arithmetic: sign extension, the 12-bit temperature living in the top of a
 * 16-bit register, the 24-bit power assembly, and the claim that one constant
 * SHUNT_CAL is correct for every shunt in both ADC ranges.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "fake_spi.h"
#include "ina239.h"

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
static double shunt_cal_eq1(double current_lsb, double shunt_ohms, ina239_range_t range)
{
    double cal = 819.2e6 * current_lsb * shunt_ohms;
    return (range == INA239_RANGE_41MV) ? cal * 4.0 : cal;
}

static ina239_handle_t make(double shunt_ohms, ina239_range_t range)
{
    fake_reset();
    const ina239_config_t cfg = {
        .dev = (spi_device_handle_t)&fake,
        .shunt_ohms = shunt_ohms,
        .range = range,
        .averaging = INA239_AVG_16,
    };
    ina239_handle_t h = NULL;
    if (ina239_create(&cfg, &h) != ESP_OK) {
        return NULL;
    }
    if (ina239_configure(h, &cfg, NULL) != ESP_OK) {
        ina239_delete(h);
        return NULL;
    }
    return h;
}

/* ------------------------------------------------------------- the SPI frame */

static void test_read_frame_shape(void)
{
    fake_reset();
    const ina239_config_t cfg = {.dev = (spi_device_handle_t)&fake,
                                 .shunt_ohms = 0.01,
                                 .range = INA239_RANGE_163MV,
                                 .averaging = INA239_AVG_1};
    ina239_handle_t h = NULL;
    expect(ina239_create(&cfg, &h) == ESP_OK, "create");

    uint16_t value = 0;
    expect(ina239_read_register(h, 0x3E, &value) == ESP_OK, "read MANUFACTURER_ID");
    expect(value == 0x5449, "MANUFACTURER_ID reads back TI");

    /* Table 7-2: 3Eh in bits 7:2, bit 1 low, read bit set. */
    expect(fake.last_frame_len == 3, "a 16-bit read is a three-byte frame");
    expect(fake.last_frame[0] == ((0x3E << 2) | 1), "read command byte");

    expect(ina239_write_register(h, 0x0C, 0x1234) == ESP_OK, "write SOVL");
    expect(fake.last_frame_len == 3, "a write is a three-byte frame");
    expect(fake.last_frame[0] == (0x0C << 2), "write command byte");
    expect(fake.reg[0x0C] == 0x1234, "the written value lands in the register");

    ina239_delete(h);
}

static void test_power_read_is_a_four_byte_frame(void)
{
    ina239_handle_t h = make(0.01, INA239_RANGE_163MV);
    expect(h != NULL, "configure against the fake");

    fake.reg[0x08] = 0x0001;  /* POWER[23:8] */
    fake.power_low = 0x02;    /* POWER[7:0]  */

    ina239_reading_t r;
    expect(ina239_read(h, &r) == ESP_OK, "read");

    /* 0x000102 = 258 counts. Equation 4: 0.2 x CURRENT_LSB x POWER. */
    const double current_lsb = 5.0e-6 / 0.01;
    expect(close_to(r.power_w, 0.2 * current_lsb * 258.0, 1e-12),
           "the 24-bit POWER register is assembled from three bytes");

    ina239_delete(h);
}

/* ------------------------------------------------------------- identity */

static void test_identity_refusals(void)
{
    fake_reset();
    fake.reg[0x3E] = 0x1234; /* something is there; it is not from TI */
    const ina239_config_t cfg = {.dev = (spi_device_handle_t)&fake,
                                 .shunt_ohms = 0.01,
                                 .range = INA239_RANGE_163MV,
                                 .averaging = INA239_AVG_1};
    ina239_handle_t h = NULL;
    ina239_create(&cfg, &h);

    ina239_config_report_t report;
    expect(ina239_configure(h, &cfg, &report) == ESP_ERR_INA239_WRONG_PART,
           "a non-TI manufacturer id is refused");
    expect(report.failed_stage == INA239_STAGE_IDENTIFY, "the stage is named");
    expect(report.manufacturer_id == 0x1234, "the observed id is quoted back");
    expect(!ina239_is_configured(h), "a refused part is not configured");

    /* A TI part that is not this one: an INA238 answers "TI" just as happily
     * and has a different shunt LSB, so it would read low by four. */
    fake.reg[0x3E] = 0x5449;
    fake.reg[0x3F] = 0x2381;
    expect(ina239_configure(h, &cfg, &report) == ESP_ERR_INA239_WRONG_DEVICE,
           "a TI part with the wrong device id is refused");
    expect(report.failed_stage == INA239_STAGE_DEVICE_ID, "the stage is named");
    expect(report.die_id == 0x238, "the observed die id is quoted back");

    fake.reg[0x3F] = 0x2391;
    expect(ina239_configure(h, &cfg, &report) == ESP_OK, "the real part is accepted");
    expect(report.die_id == 0x239 && report.revision == 1,
           "die id and revision are reported");

    ina239_delete(h);
}

static void test_no_device_is_not_success(void)
{
    fake_reset();
    fake.present = false;
    const ina239_config_t cfg = {.dev = (spi_device_handle_t)&fake,
                                 .shunt_ohms = 0.01,
                                 .range = INA239_RANGE_163MV,
                                 .averaging = INA239_AVG_1};
    ina239_handle_t h = NULL;
    ina239_create(&cfg, &h);

    ina239_config_report_t report;
    expect(ina239_configure(h, &cfg, &report) != ESP_OK, "a dead bus fails");
    expect(report.failed_stage == INA239_STAGE_PROBE, "the stage is the probe");

    ina239_delete(h);
}

static void test_a_handle_without_a_device(void)
{
    const ina239_config_t cfg = {.dev = NULL,
                                 .shunt_ohms = 0.01,
                                 .range = INA239_RANGE_163MV,
                                 .averaging = INA239_AVG_1};
    ina239_handle_t h = NULL;
    expect(ina239_create(&cfg, &h) == ESP_OK, "a handle is creatable before a bus");

    ina239_reading_t r;
    expect(ina239_configure(h, &cfg, NULL) == ESP_ERR_INVALID_STATE,
           "configure refuses without a device");
    expect(ina239_read(h, &r) == ESP_ERR_INVALID_STATE,
           "read refuses without a device");

    fake_reset();
    expect(ina239_set_device(h, (spi_device_handle_t)&fake) == ESP_OK, "set_device");
    expect(ina239_configure(h, &cfg, NULL) == ESP_OK, "configure once a bus arrives");

    ina239_delete(h);
}

/* ------------------------------------------------------- the calibration */

static void test_shunt_cal_is_one_constant_everywhere(void)
{
    const double shunts[] = {0.0005, 0.002, 0.004, 0.0162, 0.05, 0.1, 1.0};
    const ina239_range_t ranges[] = {INA239_RANGE_163MV, INA239_RANGE_41MV};

    for (size_t r = 0; r < 2; r++) {
        for (size_t i = 0; i < sizeof(shunts) / sizeof(shunts[0]); i++) {
            ina239_handle_t h = make(shunts[i], ranges[r]);
            if (!h) {
                expect(false, "configure");
                continue;
            }
            const double lsb = ina239_current_lsb(h);
            const double cal = shunt_cal_eq1(lsb, shunts[i], ranges[r]);
            expect(close_to(cal, INA239_SHUNT_CAL_VALUE, 1e-6),
                   "Equation 1 yields 4096 for this shunt and range");
            expect(fake.reg[0x02] == INA239_SHUNT_CAL_VALUE,
                   "SHUNT_CAL was programmed with it");
            ina239_delete(h);
        }
    }
}

static void test_range_is_written_before_the_calibration(void)
{
    ina239_handle_t h = make(0.01, INA239_RANGE_41MV);
    expect(h != NULL, "configure in the high-precision range");
    expect((fake.reg[0x00] & (1u << 4)) != 0, "CONFIG.ADCRANGE is set");
    expect(fake.last_write_reg == 0x02, "SHUNT_CAL is written last");
    expect(close_to(ina239_current_lsb(h), 1.25e-6 / 0.01, 1e-15),
           "CURRENT_LSB follows the range's shunt LSB");
    expect(close_to(ina239_full_scale_amps(h), 0.04096 / 0.01, 1e-9),
           "full scale is the range's span over the shunt");
    ina239_delete(h);

    h = make(0.01, INA239_RANGE_163MV);
    expect((fake.reg[0x00] & (1u << 4)) == 0, "ADCRANGE is clear in the wide range");
    expect(close_to(ina239_current_lsb(h), 5.0e-6 / 0.01, 1e-15),
           "and CURRENT_LSB follows it");
    ina239_delete(h);
}

static void test_adc_config_asks_for_everything(void)
{
    ina239_handle_t h = make(0.01, INA239_RANGE_163MV);
    /* MODE Fh: continuous bus, shunt and temperature. Anything less leaves one
     * of the five reported values frozen. */
    expect((fake.reg[0x01] >> 12) == 0xF, "MODE is continuous-everything");
    expect((fake.reg[0x01] & 0x7) == INA239_AVG_16, "AVG carries the request");
    ina239_delete(h);
}

/* --------------------------------------------------------------- decoding */

static void test_decode(void)
{
    ina239_handle_t h = make(0.01, INA239_RANGE_163MV);
    const double current_lsb = 5.0e-6 / 0.01;

    fake.reg[0x05] = 4000;   /* VBUS:    4000 x 3.125 mV = 12.5 V */
    fake.reg[0x04] = 0x8000; /* VSHUNT:  -32768 x 5 uV = -163.84 mV */
    fake.reg[0x06] = 0x0C80; /* DIETEMP: 0xC8 = 200 in 15:4, x 125 m = 25 degC */
    fake.reg[0x07] = 0xFFFF; /* CURRENT: -1 count */
    fake.reg[0x08] = 0;
    fake.power_low = 0;

    ina239_reading_t r;
    expect(ina239_read(h, &r) == ESP_OK, "read");
    expect(close_to(r.bus_v, 12.5, 1e-9), "VBUS decodes to volts");
    expect(close_to(r.shunt_v, -0.16384, 1e-12), "VSHUNT is signed");
    expect(close_to(r.temp_c, 25.0, 1e-9), "DIETEMP is 12 bits in 15:4");
    expect(close_to(r.current_a, -current_lsb, 1e-15), "CURRENT is signed");

    /* The sign bit of a negative temperature lives at bit 15, so a driver that
     * shifted before sign-extending reads -1 degC as +255.875. */
    fake.reg[0x06] = 0xFFF0; /* -1 in 15:4 */
    expect(ina239_read(h, &r) == ESP_OK, "read");
    expect(close_to(r.temp_c, -0.125, 1e-9), "a negative die temperature");

    ina239_delete(h);
}

static void test_health_flags(void)
{
    ina239_handle_t h = make(0.01, INA239_RANGE_163MV);

    ina239_reading_t r;
    expect(ina239_read(h, &r) == ESP_OK, "read");
    expect(r.trim_checksum_ok, "MEMSTAT set means the trim memory is good");
    expect(!r.math_overflow, "MATHOF clear");
    expect(r.shunt_cal_matches, "SHUNT_CAL still holds what we wrote");
    expect(r.config_intact, "the configuration is intact");

    fake.reg[0x0B] = (1u << 9) | (1u << 1); /* MATHOF and CNVRF, MEMSTAT clear */
    expect(ina239_read(h, &r) == ESP_OK, "read");
    expect(!r.trim_checksum_ok, "MEMSTAT clear is a trim checksum error");
    expect(r.math_overflow, "MATHOF is decoded");
    expect(r.conversion_ready, "CNVRF is decoded");

    ina239_delete(h);
}

/*
 * A reset part goes on answering. It just answers on the wide range, so every
 * current it reports on a 41 mV board is a quarter of the truth -- which is
 * exactly the class of failure that looks like a real measurement.
 *
 * SHUNT_CAL cannot see this: its reset value is 1000h, and 1000h is 4096, the
 * constant the calibration identity produces. CONFIG and ADC_CONFIG can, and
 * the blind spot that remains is worth pinning too, because the header promises
 * exactly where it is.
 */
static void test_a_reset_part_is_noticed(void)
{
    ina239_handle_t h = make(0.01, INA239_RANGE_41MV);
    ina239_reading_t r;

    expect(ina239_read(h, &r) == ESP_OK, "read");
    expect(r.config_intact, "intact before the reset");

    fake.reg[0x00] = 0x0000; /* CONFIG back to its reset value: the wide range */
    fake.reg[0x01] = 0xFB68; /* ADC_CONFIG likewise */
    fake.reg[0x02] = 0x1000; /* SHUNT_CAL -- indistinguishable, on its own */
    expect(ina239_read(h, &r) == ESP_OK, "read");
    expect(r.shunt_cal_matches,
           "SHUNT_CAL alone cannot see a reset: 1000h is the constant");
    expect(!r.config_intact, "but CONFIG and ADC_CONFIG can");
    ina239_delete(h);

    /*
     * The documented blind spot, and it takes both halves to reach it: the wide
     * range *and* no averaging is a request for exactly the reset defaults, so
     * a reset leaves nothing to notice. Asking for either one differently --
     * as make() does, with AVG 16 -- closes it.
     */
    fake_reset();
    const ina239_config_t defaults = {.dev = (spi_device_handle_t)&fake,
                                      .shunt_ohms = 0.01,
                                      .range = INA239_RANGE_163MV,
                                      .averaging = INA239_AVG_1};
    h = NULL;
    ina239_create(&defaults, &h);
    expect(ina239_configure(h, &defaults, NULL) == ESP_OK, "configure at the defaults");
    fake.reg[0x00] = 0x0000;
    fake.reg[0x01] = 0xFB68;
    fake.reg[0x02] = 0x1000;
    expect(ina239_read(h, &r) == ESP_OK, "read");
    expect(r.config_intact,
           "a configuration equal to the reset defaults hides a reset, as documented");
    ina239_delete(h);
}

static void test_reset_drops_the_configuration(void)
{
    ina239_handle_t h = make(0.01, INA239_RANGE_163MV);
    expect(ina239_is_configured(h), "configured");
    expect(ina239_reset(h) == ESP_OK, "reset");
    expect(!ina239_is_configured(h), "reset leaves the handle unconfigured");
    expect((fake.reg[0x00] & (1u << 15)) != 0, "CONFIG.RST was written");

    ina239_reading_t r;
    expect(ina239_read(h, &r) == ESP_ERR_INVALID_STATE,
           "reading after a reset is refused rather than scaled wrongly");
    ina239_delete(h);
}

static void test_encodings(void)
{
    expect(ina239_averaging_count(INA239_AVG_1024) == 1024, "AVG 7h is 1024");
    expect(ina239_averaging_count((ina239_averaging_t)9) == -1, "an invalid AVG");

    ina239_averaging_t avg;
    expect(ina239_averaging_from_count(64, &avg) == ESP_OK && avg == INA239_AVG_64,
           "64 maps onto 3h");
    expect(ina239_averaging_from_count(32, &avg) != ESP_OK, "32 is not an encoding");

    expect(close_to(ina239_range_full_scale_v(INA239_RANGE_41MV), 0.04096, 1e-12),
           "the high-precision range spans 40.96 mV");

    spi_device_interface_config_t dc = ina239_device_config(7, 50000000);
    expect(dc.mode == 1, "the part is clocked in SPI mode 1");
    expect(dc.clock_speed_hz == INA239_SPI_MAX_HZ, "an over-fast clock is clamped");
    expect(dc.spics_io_num == 7, "the chip select is carried through");
    dc = ina239_device_config(7, 0);
    expect(dc.clock_speed_hz == 1000000, "zero selects a default clock");
}

static void test_bad_arguments(void)
{
    fake_reset();
    const ina239_config_t base = {.dev = (spi_device_handle_t)&fake,
                                  .shunt_ohms = 0.01,
                                  .range = INA239_RANGE_163MV,
                                  .averaging = INA239_AVG_1};
    ina239_handle_t h = NULL;
    ina239_create(&base, &h);

    ina239_config_t bad = base;
    bad.shunt_ohms = 0.0;
    expect(ina239_configure(h, &bad, NULL) == ESP_ERR_INVALID_ARG,
           "a zero shunt is refused: every current would be infinite");
    bad = base;
    bad.range = (ina239_range_t)7;
    expect(ina239_configure(h, &bad, NULL) == ESP_ERR_INVALID_ARG,
           "an undefined range is refused");
    bad = base;
    bad.averaging = (ina239_averaging_t)9;
    expect(ina239_configure(h, &bad, NULL) == ESP_ERR_INVALID_ARG,
           "an undefined averaging encoding is refused");

    expect(close_to(ina239_current_lsb_for(0.0, INA239_RANGE_163MV), 0.0, 0.0),
           "no CURRENT_LSB for a zero shunt");

    ina239_delete(h);
}

int main(void)
{
    test_read_frame_shape();
    test_power_read_is_a_four_byte_frame();
    test_identity_refusals();
    test_no_device_is_not_success();
    test_a_handle_without_a_device();
    test_shunt_cal_is_one_constant_everywhere();
    test_range_is_written_before_the_calibration();
    test_adc_config_asks_for_everything();
    test_decode();
    test_health_flags();
    test_a_reset_part_is_noticed();
    test_reset_drops_the_configuration();
    test_encodings();
    test_bad_arguments();

    printf("\n%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
