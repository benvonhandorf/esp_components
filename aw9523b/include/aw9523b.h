#ifndef AW9523B_H
#define AW9523B_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Awinic AW9523B 16-bit I/O expander and LED driver.
 *
 * Two 8-bit ports. Every pin is a GPIO or a constant-current LED sink; this
 * driver covers the GPIO side.
 *
 * Returns facts rather than formatted text, so the caller decides how to report
 * them.
 */

#define ESP_ERR_AW9523B_BASE       0x3B000
/* The ID register did not read 0x23. */
#define ESP_ERR_AW9523B_WRONG_PART (ESP_ERR_AW9523B_BASE + 1)

#define AW9523B_I2C_ADDR_DEFAULT 0x58   /* AD0 and AD1 to GND; 0x58..0x5B */

#define AW9523B_PORT0 0
#define AW9523B_PORT1 1

typedef struct aw9523b_dev_t *aw9523b_handle_t;

typedef struct {
    /* May be NULL; calls needing the bus return ESP_ERR_INVALID_STATE until
     * aw9523b_set_device() supplies one. The caller owns it. */
    i2c_master_dev_handle_t dev;

    /*
     * Which pins are inputs: a 1 bit is an input, matching the part's own
     * register. Which pins drive what is a fact about the board, so it is
     * configuration here rather than a constant in the driver.
     */
    uint8_t port0_inputs;
    uint8_t port1_inputs;

    /*
     * The output levels to establish *before* any pin becomes an output.
     *
     * This ordering matters and is easy to lose: switching direction first lets
     * whatever the output register happens to hold reach the pins, which on a
     * board driving relays or FETs is an audible, and occasionally destructive,
     * glitch at every reset.
     */
    uint8_t port0_initial;
    uint8_t port1_initial;

    /*
     * Drive port 0 push-pull rather than open-drain. Port 1 is always push-pull
     * in GPIO mode; port 0 is open-drain unless this is set, which surprises
     * anyone expecting it to drive a load high.
     */
    bool port0_push_pull;
} aw9523b_config_t;

typedef enum {
    AW9523B_STAGE_NONE = 0,
    AW9523B_STAGE_IDENTIFY,
    AW9523B_STAGE_PRESET,      /* establishing output levels */
    AW9523B_STAGE_CONFIGURE,   /* drive mode and directions */
} aw9523b_stage_t;

typedef struct {
    aw9523b_stage_t failed_stage;
    uint8_t chip_id;           /* 0x23 for this part */
} aw9523b_report_t;

esp_err_t aw9523b_create(const aw9523b_config_t *cfg, aw9523b_handle_t *out,
                         aw9523b_report_t *report);
void      aw9523b_delete(aw9523b_handle_t handle);
esp_err_t aw9523b_set_device(aw9523b_handle_t handle, i2c_master_dev_handle_t dev,
                             aw9523b_report_t *report);

esp_err_t aw9523b_write_port(aw9523b_handle_t handle, uint8_t port, uint8_t value);
esp_err_t aw9523b_read_port(aw9523b_handle_t handle, uint8_t port, uint8_t *value);
esp_err_t aw9523b_set_pin(aw9523b_handle_t handle, uint8_t port, uint8_t pin, bool high);
esp_err_t aw9523b_get_pin(aw9523b_handle_t handle, uint8_t port, uint8_t pin, bool *high);

/*
 * Clear a pending interrupt by reading both input ports.
 *
 * Call from whatever services the INT pin. This driver registers no handler:
 * which pin the part is wired to, and how interrupts are dispatched, are facts
 * about the board.
 */
esp_err_t aw9523b_clear_interrupt(aw9523b_handle_t handle);

const char *aw9523b_stage_name(aw9523b_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* AW9523B_H */
