#ifndef LM75BDP_H
#define LM75BDP_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NXP LM75B temperature sensor and thermal watchdog.
 *
 * Returns facts rather than formatted text, so the caller decides how to report
 * them.
 */

#define ESP_ERR_LM75BDP_BASE       0x39000
/* The configuration read-back did not match: the address answered, but not as an
 * LM75B. */
#define ESP_ERR_LM75BDP_WRONG_PART (ESP_ERR_LM75BDP_BASE + 1)

#define LM75BDP_I2C_ADDR_DEFAULT 0x48   /* A0-A2 to GND */

typedef struct lm75bdp_dev_t *lm75bdp_handle_t;

typedef struct {
    /* May be NULL; calls needing the bus return ESP_ERR_INVALID_STATE until
     * lm75bdp_set_device() supplies one. The caller owns it. */
    i2c_master_dev_handle_t dev;
} lm75bdp_config_t;

typedef struct {
    float temperature_C;
} lm75bdp_reading_t;

typedef enum {
    LM75BDP_STAGE_NONE = 0,
    LM75BDP_STAGE_IDENTIFY,   /* configuration read-back */
    LM75BDP_STAGE_CONFIGURE,
    LM75BDP_STAGE_THRESHOLD,
} lm75bdp_stage_t;

typedef struct {
    lm75bdp_stage_t failed_stage;
    /* The thresholds actually programmed, which are quantised to 0.5 C and may
     * differ from what was asked for. */
    float tos_C;
    float thyst_C;
} lm75bdp_report_t;

esp_err_t lm75bdp_create(const lm75bdp_config_t *cfg, lm75bdp_handle_t *out,
                         lm75bdp_report_t *report);
void      lm75bdp_delete(lm75bdp_handle_t handle);
esp_err_t lm75bdp_set_device(lm75bdp_handle_t handle, i2c_master_dev_handle_t dev,
                             lm75bdp_report_t *report);

esp_err_t lm75bdp_read(lm75bdp_handle_t handle, lm75bdp_reading_t *out);

/*
 * Program the thermal watchdog. The OS output asserts above `tos_C` and releases
 * below `thyst_C`; the gap between them is the hysteresis, and setting them equal
 * makes the output chatter around the threshold.
 *
 * Both are quantised to 0.5 C and clamped to the part's -128..+127.5 C range; the
 * report says what was actually programmed.
 */
esp_err_t lm75bdp_set_thresholds(lm75bdp_handle_t handle, float tos_C, float thyst_C,
                                 lm75bdp_report_t *report);

const char *lm75bdp_stage_name(lm75bdp_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* LM75BDP_H */
