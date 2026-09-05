/*
 * TI INA237 current / voltage / power monitor.
 *
 * Register addresses and every scaling constant here come from the INA237
 * datasheet, SBOSA20A (revised May 2022). Section references are given where
 * the number is not self-evident.
 *
 * This driver formats no text. Calls return esp_err_t and fill out-structs with
 * facts -- which stage of the configuration failed, what the part actually
 * answered when it was probed, whether the device has been reset since it was
 * calibrated -- so the caller can report them in whatever way suits it.
 */
#ifndef INA237_H
#define INA237_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The A0/A1 pins select one of 16 addresses (Table 7-2). */
#define INA237_ADDR_FIRST 0x40
#define INA237_ADDR_LAST  0x4F

/*
 * Table 8-1, ADC Full Scale Values, at ADCRANGE = 0. This driver leaves
 * ADCRANGE at its reset value and never writes CONFIG, so one set of constants
 * covers every part it talks to.
 */
#define INA237_SHUNT_FULL_SCALE_V 0.16384

/* Table 7-16: MANUFACTURER_ID reads back the ASCII "TI". */
#define INA237_MANUFACTURER_ID_TI 0x5449

/*
 * Equation 1: SHUNT_CAL = 819.2e6 x CURRENT_LSB x RSHUNT.
 *
 * Picking the maximum expected current so that it exactly fills the ADC range
 * gives CURRENT_LSB = SHUNT_FULL_SCALE_V / RSHUNT / 2^15, which is simply the
 * shunt LSB divided by the resistance. Substituting that back into Equation 1
 * cancels RSHUNT entirely, so SHUNT_CAL is the same constant for every shunt
 * value -- and happens to equal the register's own reset value of 0x1000.
 *
 * Exported because reading it back is how you find out the part has been reset
 * since it was configured, which is a fact a caller wants to report.
 */
#define INA237_SHUNT_CAL_VALUE 4096

/* Error codes. The base is outside the range ESP-IDF assigns to itself. */
#define ESP_ERR_INA237_BASE 0x31000
/*
 * MANUFACTURER_ID answered, but not with "TI". Distinct from a transfer error
 * because something is present on the address and talking -- it is just not
 * this part, and telling the two apart is most of what a bringup tool is for.
 */
#define ESP_ERR_INA237_WRONG_PART (ESP_ERR_INA237_BASE + 1)

/* Opaque per-device handle. One INA237, at one address, on one bus. */
typedef struct ina237_dev_t *ina237_handle_t;

/*
 * `dev` may be NULL: a handle can be created before there is a bus, and every
 * call that would talk to the part returns ESP_ERR_INVALID_STATE until one
 * arrives via ina237_set_device().
 *
 * `shunt_ohms` must be positive. It is the one board fact the driver cannot
 * discover -- the part measures a voltage across a resistor it knows nothing
 * about -- so there is no default here; the caller supplies one.
 */
typedef struct {
    i2c_master_dev_handle_t dev;
    double shunt_ohms;
} ina237_config_t;

/* Which step of ina237_configure() failed. */
typedef enum {
    INA237_STAGE_NONE = 0,      /* success */
    INA237_STAGE_PROBE,         /* reading MANUFACTURER_ID */
    INA237_STAGE_IDENTIFY,      /* it answered, but it is not an INA237 */
    INA237_STAGE_SHUNT_CAL,     /* writing SHUNT_CAL */
} ina237_stage_t;

typedef struct {
    ina237_stage_t failed_stage;
    /*
     * What MANUFACTURER_ID actually read. Only meaningful at
     * INA237_STAGE_IDENTIFY, and carried because quoting the wrong value back
     * is what lets someone recognise the part they really fitted.
     */
    uint16_t manufacturer_id;
    /* Derived from the shunt, valid on success. */
    double current_lsb;         /* amperes per CURRENT register count */
    double full_scale_amps;
} ina237_config_report_t;

/*
 * One pass over the measurement registers.
 *
 * The three health flags are decoded rather than raw because each is a
 * different kind of problem: MEMSTAT means the trim memory is corrupt and
 * nothing can be trusted, MATHOF means current and power specifically are
 * invalid while bus voltage is still good, and a SHUNT_CAL mismatch means the
 * part was reset out from under the configuration and needs configuring again.
 * `diag` is kept as well so a caller can show the register itself.
 */
typedef struct {
    double bus_v;
    double shunt_v;
    double temp_c;
    double current_a;
    double power_w;

    uint16_t shunt_cal;
    uint16_t diag;

    bool trim_checksum_ok;   /* DIAG_ALRT.MEMSTAT: 1 = normal */
    bool math_overflow;      /* DIAG_ALRT.MATHOF */
    bool shunt_cal_matches;  /* SHUNT_CAL still holds what configure() wrote */
} ina237_reading_t;

/*
 * Create a handle. `config` must not be NULL; `config->dev` may be.
 *
 * This does not touch the bus -- nothing is probed and nothing is written until
 * ina237_configure().
 */
esp_err_t ina237_create(const ina237_config_t *config, ina237_handle_t *out);

/* Free the handle. Does NOT delete the I2C device handle, which the caller owns. */
esp_err_t ina237_delete(ina237_handle_t handle);

/*
 * Re-point the handle at a device.
 *
 * Callers whose I2C layer caches device handles must do this before each use:
 * a cache that recycles entries, or a bus that is torn down and rebuilt, leaves
 * a previously held handle dangling.
 */
esp_err_t ina237_set_device(ina237_handle_t handle, i2c_master_dev_handle_t dev);

/*
 * Confirm an INA237 is really there and program its calibration.
 *
 * Probes MANUFACTURER_ID, writes SHUNT_CAL, and records the shunt value so
 * later readings can be scaled. Re-running is how you change the shunt.
 *
 * `report` may be NULL. On failure it names the stage; on success it carries
 * the derived CURRENT_LSB and full-scale current, which are worth reporting
 * because they are what the shunt choice actually bought.
 */
esp_err_t ina237_configure(ina237_handle_t handle, double shunt_ohms,
                           ina237_config_report_t *report);

/* True once ina237_configure() has succeeded on this handle. */
bool ina237_is_configured(ina237_handle_t handle);

/*
 * Read bus voltage, shunt voltage, die temperature, current, power, and the
 * health registers.
 *
 * Every value is read in its own transaction: the INA237 needs the register
 * pointer written before each read and does not auto-increment across
 * registers (section 7.5.1.1).
 */
esp_err_t ina237_read(ina237_handle_t handle, ina237_reading_t *out);

/* The configured shunt, and what it implies. Zero before ina237_configure(). */
double ina237_shunt_ohms(ina237_handle_t handle);
double ina237_current_lsb(ina237_handle_t handle);
double ina237_full_scale_amps(ina237_handle_t handle);

/*
 * CURRENT_LSB for a shunt, without a handle. Lets a caller show what a shunt
 * value would buy before committing to it.
 */
double ina237_current_lsb_for(double shunt_ohms);

/* Read one register, for a caller that wants to show the raw map. */
esp_err_t ina237_read_register(ina237_handle_t handle, uint8_t reg, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif
