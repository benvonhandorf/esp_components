#ifndef INA219_H
#define INA219_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TI INA219 bidirectional current, voltage and power monitor.
 *
 * Returns facts rather than formatted text, so the caller decides how to report
 * them.
 */

/* Distinct from every other driver's base, so esp_err_to_name() cannot
 * attribute a failure to the wrong part. */
#define ESP_ERR_INA219_BASE       0x38000
/*
 * The part did not respond as an INA219.
 *
 * Unlike the INA226 there are no manufacturer or die ID registers to read, so
 * this is inferred: the calibration register is written and read back, which a
 * part that is not there -- or is not an INA219 -- will not reproduce.
 */
#define ESP_ERR_INA219_WRONG_PART (ESP_ERR_INA219_BASE + 1)
/* The requested shunt and full-scale current cannot be represented: the range
 * needs more shunt drop than the PGA can see, or so little that the calibration
 * overflows 16 bits. See max_current_a. */
#define ESP_ERR_INA219_BAD_RANGE  (ESP_ERR_INA219_BASE + 2)

#define INA219_I2C_ADDR_DEFAULT 0x40   /* A0 and A1 to GND */

typedef struct ina219_dev_t *ina219_handle_t;

typedef struct {
    /*
     * The I2C device to talk to. May be NULL: every call that needs the bus then
     * returns ESP_ERR_INVALID_STATE until ina219_set_device() supplies one.
     *
     * The caller owns it. A bus that is torn down and rebuilt invalidates the
     * handle, so whoever owns the bus must re-point this driver at the new one
     * rather than this driver caching a handle that can dangle.
     */
    i2c_master_dev_handle_t dev;

    /* The sense resistor actually fitted, in ohms. */
    float shunt_ohms;

    /*
     * The largest current to be measured, in amps. Together with the shunt this
     * fixes the resolution: the current register is signed 15-bit, so the LSB is
     * this divided by 32768. Asking for more range than needed throws away
     * resolution; asking for less saturates.
     *
     * This times shunt_ohms must not exceed 320 mV, the widest range the PGA
     * offers and the one this driver programs. Beyond that the front end
     * saturates whatever the calibration says -- 10 A across 0.1 ohm reads as
     * 3.2 A -- so the request is refused with ESP_ERR_INA219_BAD_RANGE rather
     * than reporting a range the part cannot reach. Too small a range is refused
     * for the opposite reason: below about 20 mV of full-scale shunt drop the
     * calibration no longer fits 16 bits.
     */
    float max_current_a;
} ina219_config_t;

typedef struct {
    float shunt_voltage;   /* V, signed */
    float bus_voltage;     /* V */
    float current;         /* A, signed: negative is current flowing the other way */
    float power;           /* W */
} ina219_reading_t;

/* Which step of configuration failed, so the caller can say what to do rather
 * than reporting a generic error. */
typedef enum {
    INA219_STAGE_NONE = 0,
    INA219_STAGE_RANGE,      /* the shunt and range cannot be represented */
    INA219_STAGE_IDENTIFY,   /* the calibration read-back did not match */
    INA219_STAGE_RESET,
    INA219_STAGE_CONFIGURE,
    INA219_STAGE_CALIBRATE,
} ina219_stage_t;

typedef struct {
    ina219_stage_t failed_stage;
    float    current_lsb_a;     /* the resolution actually programmed */
    float    full_scale_a;      /* the largest current it can represent */
    uint16_t calibration;       /* the register value written; always even, as
                                 * the register's low bit is void */
} ina219_report_t;

/*
 * Create a handle and, if a device was supplied, configure the part.
 *
 * `report` may be NULL; when given it is filled in on success and failure alike,
 * so a caller can report the resolution it actually got -- which is rarely the
 * round number that was asked for.
 */
esp_err_t ina219_create(const ina219_config_t *cfg, ina219_handle_t *out,
                        ina219_report_t *report);

/* Frees the handle. Never touches the caller's I2C device. */
void ina219_delete(ina219_handle_t handle);

/* Point an existing handle at a device, and configure the part. Use after a bus
 * has been rebuilt, which invalidates every device handle taken from it. */
esp_err_t ina219_set_device(ina219_handle_t handle, i2c_master_dev_handle_t dev,
                            ina219_report_t *report);

esp_err_t ina219_read(ina219_handle_t handle, ina219_reading_t *out);

/*
 * Does nothing, and always succeeds. Provided so a caller can treat the current
 * monitors uniformly.
 *
 * The INA219 has neither an alert output nor a mask/enable register to clear:
 * its register map is six registers, 00h to 05h, and its pinout is IN+, IN-,
 * GND, VS, SCL, SDA, A0 and A1. The INA226 has both, which is where this call
 * earns its keep. The nearest thing here is the OVF flag in the bus voltage
 * register, which the part maintains itself.
 */
esp_err_t ina219_clear_alert(ina219_handle_t handle);

/* Human-readable stage name, for reporting. Never NULL. */
const char *ina219_stage_name(ina219_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* INA219_H */
