/*
 * TI INA239 current / voltage / power monitor, SPI.
 *
 * Register addresses, frame format and every scaling constant here come from
 * the INA239 datasheet, SLYS027A (revised May 2022). Section references are
 * given where the number is not self-evident.
 *
 * The part is the SPI sibling of the INA238: same measurement set, same
 * arithmetic, a different transport. The transport is the whole of the
 * difference and it is not a small one -- there is no address, so a bus carries
 * as many of these as it has chip selects, and a frame is a command byte
 * followed by the register rather than a pointer write and a restart.
 *
 * This driver formats no text. Calls return esp_err_t and fill out-structs with
 * facts -- which stage of the configuration failed, what the part actually
 * answered when it was probed, whether the device has been reset since it was
 * calibrated -- so the caller can report them in whatever way suits it.
 */
#ifndef INA239_H
#define INA239_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How the part must be clocked.
 *
 * Section 7.5.1: SCLK idles low, MOSI is sampled on the falling edge and MISO
 * is shifted out on the rising edge -- which is SPI mode 1 (CPOL 0, CPHA 1) --
 * at up to 10 Mbit/s. Both are properties of the silicon rather than of the
 * board, so they are published here instead of being left for each application
 * to rediscover; ina239_device_config() fills them in.
 */
#define INA239_SPI_MODE   1
#define INA239_SPI_MAX_HZ 10000000

/* Table 7-20: MANUFACTURER_ID reads back the ASCII "TI". */
#define INA239_MANUFACTURER_ID_TI 0x5449
/* Table 7-21: DEVICE_ID is 239h in bits 15:4 with the revision in 3:0. */
#define INA239_DEVICE_ID 0x239

/*
 * Table 8-1, ADC full scale, for each setting of CONFIG.ADCRANGE. Bus voltage
 * is 0-85 V in both.
 */
#define INA239_SHUNT_FULL_SCALE_V_LOW  0.16384 /* ADCRANGE = 0, 5 uV/LSB    */
#define INA239_SHUNT_FULL_SCALE_V_HIGH 0.04096 /* ADCRANGE = 1, 1.25 uV/LSB */

/*
 * Equation 1: SHUNT_CAL = 819.2e6 x CURRENT_LSB x RSHUNT, and the value must be
 * multiplied by 4 when ADCRANGE = 1.
 *
 * Picking the maximum expected current so that it exactly fills the ADC range
 * gives CURRENT_LSB = SHUNT_LSB_V / RSHUNT, which is simply the shunt LSB
 * divided by the resistance. Substituting that back into Equation 1 cancels
 * RSHUNT entirely, so SHUNT_CAL is the same constant for every shunt value.
 *
 * It is also the same constant in both ranges, which is less obvious and worth
 * stating: halving the LSB four times over -- 5 uV to 1.25 uV -- divides the
 * product by four, and the ADCRANGE = 1 rule multiplies it straight back.
 * 819.2e6 x 5e-6 = 4096, and 819.2e6 x 1.25e-6 x 4 = 4096.
 *
 * Exported because reading it back is how you find out the part has been reset
 * since it was configured, which is a fact a caller wants to report.
 */
#define INA239_SHUNT_CAL_VALUE 4096

/* Error codes. The base is outside the range ESP-IDF assigns to itself. */
#define ESP_ERR_INA239_BASE 0x3E000
/*
 * MANUFACTURER_ID answered, but not with "TI". Distinct from a transfer error
 * because something is on the chip select and talking -- it is just not this
 * part, and telling the two apart is most of what a bringup tool is for.
 */
#define ESP_ERR_INA239_WRONG_PART (ESP_ERR_INA239_BASE + 1)
/*
 * MANUFACTURER_ID said TI but DEVICE_ID did not say 239h. Separate from
 * WRONG_PART because it names a much more likely mistake -- an INA228, INA237
 * or INA238 fitted instead -- and the fix is different.
 */
#define ESP_ERR_INA239_WRONG_DEVICE (ESP_ERR_INA239_BASE + 2)

/* CONFIG.ADCRANGE. The enumerator value is the register encoding. */
typedef enum {
    /* +/-163.84 mV across IN+/IN-, 5 uV per count. The reset default. */
    INA239_RANGE_163MV = 0,
    /* +/-40.96 mV, 1.25 uV per count: four times the resolution, a quarter of
     * the span. Correct whenever the shunt was sized so the full current fits. */
    INA239_RANGE_41MV = 1,
} ina239_range_t;

/* ADC_CONFIG.AVG. The enumerator value is the register encoding. */
typedef enum {
    INA239_AVG_1 = 0,
    INA239_AVG_4 = 1,
    INA239_AVG_16 = 2,
    INA239_AVG_64 = 3,
    INA239_AVG_128 = 4,
    INA239_AVG_256 = 5,
    INA239_AVG_512 = 6,
    INA239_AVG_1024 = 7,
} ina239_averaging_t;

/* Encoding tables, exported so a caller can render the legal set, or map a
 * number a user typed onto an encoding, without keeping a second copy. */
int ina239_averaging_count(ina239_averaging_t avg);  /* 1..1024, -1 if invalid */
esp_err_t ina239_averaging_from_count(int count, ina239_averaging_t *out);
extern const ina239_averaging_t ina239_averagings[8];

double ina239_range_full_scale_v(ina239_range_t range); /* 0 if invalid */

/* Opaque per-device handle. One INA239, on one chip select. */
typedef struct ina239_dev_t *ina239_handle_t;

/*
 * `dev` may be NULL: a handle can be created before there is a bus, and every
 * call that would talk to the part returns ESP_ERR_INVALID_STATE until one
 * arrives via ina239_set_device().
 *
 * `shunt_ohms` must be positive. It is the one board fact the driver cannot
 * discover -- the part measures a voltage across a resistor it knows nothing
 * about -- so there is no default here; the caller supplies one.
 *
 * `range` and `averaging` are written to the device by ina239_configure().
 * Unlike the I2C parts in this repository, this driver does write CONFIG: the
 * range is half of what the shunt choice buys, and leaving it at its reset
 * value would throw away two bits on any board whose shunt was sized properly.
 */
typedef struct {
    spi_device_handle_t dev;
    double shunt_ohms;
    ina239_range_t range;
    ina239_averaging_t averaging;
} ina239_config_t;

/*
 * Fill in an spi_device_interface_config_t for this part.
 *
 * The bus, and the device handle made on it, stay the application's -- but the
 * clock mode is a property of the silicon, and a mode-0 handle produces
 * readings that are plausible, stable and wrong rather than an error. So the
 * struct is filled here and added to the bus there:
 *
 *     spi_device_interface_config_t dc = ina239_device_config(CS_GPIO, 1000000);
 *     spi_bus_add_device(SPI2_HOST, &dc, &dev);
 *
 * `clock_hz` is clamped to INA239_SPI_MAX_HZ; 0 selects 1 MHz.
 */
spi_device_interface_config_t ina239_device_config(int cs_gpio, int clock_hz);

/* Which step of ina239_configure() failed. */
typedef enum {
    INA239_STAGE_NONE = 0,   /* success */
    INA239_STAGE_PROBE,      /* reading MANUFACTURER_ID */
    INA239_STAGE_IDENTIFY,   /* it answered, but not with "TI" */
    INA239_STAGE_DEVICE_ID,  /* it is a TI part, but not an INA239 */
    INA239_STAGE_CONFIG,     /* writing CONFIG (the ADC range) */
    INA239_STAGE_ADC_CONFIG, /* writing ADC_CONFIG (mode and averaging) */
    INA239_STAGE_SHUNT_CAL,  /* writing SHUNT_CAL */
} ina239_stage_t;

typedef struct {
    ina239_stage_t failed_stage;
    /*
     * What the two identity registers actually read. Only meaningful at
     * INA239_STAGE_IDENTIFY and INA239_STAGE_DEVICE_ID respectively, and
     * carried because quoting the wrong value back is what lets someone
     * recognise the part they really fitted.
     */
    uint16_t manufacturer_id;
    uint16_t device_id;
    uint16_t die_id;  /* device_id >> 4 */
    uint8_t revision; /* device_id & 0xF */
    /* Derived from the shunt and the range, valid on success. */
    double current_lsb; /* amperes per CURRENT register count */
    double full_scale_amps;
} ina239_config_report_t;

/*
 * One pass over the measurement registers.
 *
 * The health flags are decoded rather than raw because each is a different kind
 * of problem: MEMSTAT means the trim memory is corrupt and nothing can be
 * trusted, MATHOF means current and power specifically are invalid while bus
 * voltage is still good, and `config_intact` false means the part was reset out
 * from under the configuration and is measuring on a different range than the
 * arithmetic here assumes. The raw registers are kept as well so a caller can
 * show them.
 */
typedef struct {
    double bus_v;
    double shunt_v;
    double temp_c;
    double current_a;
    double power_w;

    uint16_t config;
    uint16_t adc_config;
    uint16_t shunt_cal;
    uint16_t diag;

    bool trim_checksum_ok; /* DIAG_ALRT.MEMSTAT: 1 = normal */
    bool math_overflow;    /* DIAG_ALRT.MATHOF */
    bool conversion_ready; /* DIAG_ALRT.CNVRF */
    /*
     * All three configuration registers still hold what ina239_configure()
     * wrote.
     *
     * Read this with its one blind spot in mind. SHUNT_CAL's reset value is
     * 1000h, and 1000h is 4096 -- the very constant the calibration identity
     * produces -- so that register alone can never tell a configured part from
     * a freshly reset one. CONFIG and ADC_CONFIG close most of the gap, but not
     * all of it: a caller that asks for the wide range with no averaging is
     * asking for exactly the reset defaults, and then all three registers match
     * whether or not the part has been reset. Every other combination is
     * detected. Ask for averaging, or the 41 mV range, and this flag means what
     * it says.
     */
    bool config_intact;
    bool shunt_cal_matches; /* the SHUNT_CAL half of it, on its own */
} ina239_reading_t;

/*
 * Create a handle. `config` must not be NULL; `config->dev` may be.
 *
 * This does not touch the bus -- nothing is probed and nothing is written until
 * ina239_configure().
 */
esp_err_t ina239_create(const ina239_config_t *config, ina239_handle_t *out);

/* Free the handle. Does NOT remove the SPI device, which the caller owns. */
esp_err_t ina239_delete(ina239_handle_t handle);

/*
 * Re-point the handle at a device.
 *
 * Callers whose SPI layer caches device handles must do this before each use: a
 * cache that recycles entries, or a bus that is torn down and rebuilt, leaves a
 * previously held handle dangling.
 */
esp_err_t ina239_set_device(ina239_handle_t handle, spi_device_handle_t dev);

/*
 * Confirm an INA239 is really there, set the ADC range and averaging, and
 * program the calibration.
 *
 * Probes MANUFACTURER_ID and DEVICE_ID, writes CONFIG, ADC_CONFIG and
 * SHUNT_CAL, and records the shunt value so later readings can be scaled.
 * Re-running is how you change the shunt, the range or the averaging.
 *
 * `report` may be NULL. On failure it names the stage; on success it carries
 * the derived CURRENT_LSB and full-scale current, which are worth reporting
 * because they are what the shunt and range choice actually bought.
 */
esp_err_t ina239_configure(ina239_handle_t handle, const ina239_config_t *config,
                           ina239_config_report_t *report);

/* True once ina239_configure() has succeeded on this handle. */
bool ina239_is_configured(ina239_handle_t handle);

/*
 * Read bus voltage, shunt voltage, die temperature, current, power, and the
 * health registers.
 *
 * Every value is its own SPI frame: the frame carries the register address, so
 * there is no burst read and nothing auto-increments (section 7.5.1.1).
 */
esp_err_t ina239_read(ina239_handle_t handle, ina239_reading_t *out);

/* Software reset: CONFIG.RST. The part comes back at its reset defaults, so the
 * handle is left unconfigured and ina239_configure() must be run again. */
esp_err_t ina239_reset(ina239_handle_t handle);

/* The configured shunt and range, and what they imply. Zero before
 * ina239_configure(). */
double ina239_shunt_ohms(ina239_handle_t handle);
double ina239_current_lsb(ina239_handle_t handle);
double ina239_full_scale_amps(ina239_handle_t handle);
ina239_range_t ina239_range(ina239_handle_t handle);

/*
 * CURRENT_LSB for a shunt and range, without a handle. Lets a caller show what
 * a shunt value would buy before committing to it.
 */
double ina239_current_lsb_for(double shunt_ohms, ina239_range_t range);

/* Read or write one register, for a caller that wants to show the raw map or
 * drive the alert thresholds this driver does not model. */
esp_err_t ina239_read_register(ina239_handle_t handle, uint8_t reg, uint16_t *out);
esp_err_t ina239_write_register(ina239_handle_t handle, uint8_t reg, uint16_t value);

/* One row of the device's register map, for a diagnostic dump. */
typedef struct {
    uint8_t reg;
    const char *name;
    uint8_t bits; /* 16, or 24 for POWER */
} ina239_register_info_t;

const ina239_register_info_t *ina239_register_map(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* INA239_H */
