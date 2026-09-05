#ifndef INA226_H
#define INA226_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TI INA226 bidirectional current, voltage and power monitor.
 *
 * Returns facts rather than formatted text, so the caller decides how to report
 * them.
 */

/* Distinct from every other driver's base, so esp_err_to_name() cannot
 * attribute a failure to the wrong part. */
#define ESP_ERR_INA226_BASE       0x37000
/* The manufacturer or die ID is not TI's INA226: usually a different part at
 * this address, or an address that answers but is something else entirely. */
#define ESP_ERR_INA226_WRONG_PART (ESP_ERR_INA226_BASE + 1)
/* The requested shunt and full-scale current cannot be represented. */
#define ESP_ERR_INA226_BAD_RANGE  (ESP_ERR_INA226_BASE + 2)

#define INA226_I2C_ADDR_DEFAULT 0x40   /* A0 and A1 to GND */

typedef struct ina226_dev_t *ina226_handle_t;

/* Samples averaged per reading. More averaging trades response time for noise. */
typedef enum {
    INA226_AVG_1 = 0, INA226_AVG_4, INA226_AVG_16, INA226_AVG_64,
    INA226_AVG_128, INA226_AVG_256, INA226_AVG_512, INA226_AVG_1024,
} ina226_averaging_t;

typedef struct {
    /*
     * The I2C device to talk to. May be NULL: every call that needs the bus then
     * returns ESP_ERR_INVALID_STATE until ina226_set_device() supplies one.
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
     */
    float max_current_a;

    /* 0 selects INA226_AVG_16, which is what the reference design used. */
    ina226_averaging_t averaging;
} ina226_config_t;

typedef struct {
    float shunt_voltage;   /* V, signed */
    float bus_voltage;     /* V */
    float current;         /* A, signed: negative is current flowing the other way */
    float power;           /* W */
} ina226_reading_t;

/* Which step of configuration failed, so the caller can say what to do rather
 * than reporting a generic error. */
typedef enum {
    INA226_STAGE_NONE = 0,
    INA226_STAGE_RANGE,      /* the shunt and range cannot be represented */
    INA226_STAGE_IDENTIFY,   /* reading the manufacturer and die IDs */
    INA226_STAGE_RESET,
    INA226_STAGE_CONFIGURE,
    INA226_STAGE_CALIBRATE,
} ina226_stage_t;

typedef struct {
    ina226_stage_t failed_stage;
    uint16_t manufacturer_id;   /* 0x5449, "TI" */
    uint16_t die_id;            /* 0x2260 */
    float    current_lsb_a;     /* the resolution actually programmed */
    float    full_scale_a;      /* the largest current it can represent */
    uint16_t calibration;       /* the register value written */
} ina226_report_t;

/*
 * Create a handle and, if a device was supplied, configure the part.
 *
 * `report` may be NULL; when given it is filled in on success and failure alike,
 * so a caller can report the resolution it actually got -- which is rarely the
 * round number that was asked for.
 */
esp_err_t ina226_create(const ina226_config_t *cfg, ina226_handle_t *out,
                        ina226_report_t *report);

/* Frees the handle. Never touches the caller's I2C device. */
void ina226_delete(ina226_handle_t handle);

/* Point an existing handle at a device, and configure the part. Use after a bus
 * has been rebuilt, which invalidates every device handle taken from it. */
esp_err_t ina226_set_device(ina226_handle_t handle, i2c_master_dev_handle_t dev,
                            ina226_report_t *report);

esp_err_t ina226_read(ina226_handle_t handle, ina226_reading_t *out);

/*
 * Clear the alert flags by reading the mask/enable register.
 *
 * Call from whatever services the ALERT pin. This driver does not register an
 * interrupt handler of its own: which pin the part is wired to, and how
 * interrupts are dispatched, are facts about the board.
 */
esp_err_t ina226_clear_alert(ina226_handle_t handle);

/* Human-readable stage name, for reporting. Never NULL. */
const char *ina226_stage_name(ina226_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* INA226_H */
