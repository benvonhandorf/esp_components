#ifndef PI4IOE5V6408_H
#define PI4IOE5V6408_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Diodes PI4IOE5V6408 8-bit I/O expander with interrupt, pull configuration and
 * 5 V tolerant inputs.
 *
 * Returns facts rather than formatted text, so the caller decides how to report
 * them. Pins are numbered, not named: what P4 drives is a fact about the board,
 * not about the part.
 */

#define ESP_ERR_PI4IOE5V6408_BASE       0x3C000
/* The device ID field did not match. */
#define ESP_ERR_PI4IOE5V6408_WRONG_PART (ESP_ERR_PI4IOE5V6408_BASE + 1)

#define PI4IOE5V6408_I2C_ADDR_DEFAULT 0x43   /* ADDR to GND; 0x44 when pulled high */

typedef struct pi4ioe5v6408_dev_t *pi4ioe5v6408_handle_t;

typedef struct {
    /* May be NULL; calls needing the bus return ESP_ERR_INVALID_STATE until
     * pi4ioe5v6408_set_device() supplies one. The caller owns it. */
    i2c_master_dev_handle_t dev;

    /* A 1 bit makes the pin an output, matching the part's own register. */
    uint8_t outputs;

    /* Output levels established before any pin becomes an output, so a pin does
     * not glitch to whatever the register happened to hold. */
    uint8_t initial;

    /* Pull resistors, for input pins. `pull_enable` turns one on; `pull_up`
     * chooses its direction (1 = up). A switch to ground with no external
     * pull-up needs both bits set, which is the usual case for buttons. */
    uint8_t pull_enable;
    uint8_t pull_up;

    /* A 1 bit reports changes on that pin through the INT output. */
    uint8_t interrupt_on;
} pi4ioe5v6408_config_t;

typedef enum {
    PI4IOE5V6408_STAGE_NONE = 0,
    PI4IOE5V6408_STAGE_IDENTIFY,
    PI4IOE5V6408_STAGE_PRESET,
    PI4IOE5V6408_STAGE_CONFIGURE,
} pi4ioe5v6408_stage_t;

typedef struct {
    pi4ioe5v6408_stage_t failed_stage;
    uint8_t control_register;
} pi4ioe5v6408_report_t;

esp_err_t pi4ioe5v6408_create(const pi4ioe5v6408_config_t *cfg,
                              pi4ioe5v6408_handle_t *out,
                              pi4ioe5v6408_report_t *report);
void      pi4ioe5v6408_delete(pi4ioe5v6408_handle_t handle);
esp_err_t pi4ioe5v6408_set_device(pi4ioe5v6408_handle_t handle,
                                  i2c_master_dev_handle_t dev,
                                  pi4ioe5v6408_report_t *report);

esp_err_t pi4ioe5v6408_write_port(pi4ioe5v6408_handle_t handle, uint8_t value);
esp_err_t pi4ioe5v6408_read_port(pi4ioe5v6408_handle_t handle, uint8_t *value);
esp_err_t pi4ioe5v6408_set_pin(pi4ioe5v6408_handle_t handle, uint8_t pin, bool high);
esp_err_t pi4ioe5v6408_get_pin(pi4ioe5v6408_handle_t handle, uint8_t pin, bool *high);

/* Which pins changed since the last read. Reading clears the latch, so this both
 * reports and acknowledges. Call from whatever services the INT pin. */
esp_err_t pi4ioe5v6408_read_interrupt_status(pi4ioe5v6408_handle_t handle,
                                             uint8_t *changed);

const char *pi4ioe5v6408_stage_name(pi4ioe5v6408_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* PI4IOE5V6408_H */
