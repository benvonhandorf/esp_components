#ifndef INT_DISPATCH_H
#define INT_DISPATCH_H

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dispatch for a shared, wired-AND interrupt line.
 *
 * Several I2C parts commonly share one open-drain INT pin, so an edge says only
 * that *something* wants attention. Each handler checks its own status register,
 * and the line stays asserted until every one of them has been serviced.
 *
 * Handlers run on a dedicated task, not in the ISR: clearing an interrupt on an
 * I2C part means a bus transaction, which cannot happen in interrupt context.
 */

typedef void (*int_dispatch_cb_t)(void *ctx);

typedef struct {
    /* The shared INT pin. A fact about the board, which is why it is a parameter
     * -- the original had GPIO 14 compiled in. */
    gpio_num_t pin;

    /* Enable the internal pull-up. An open-drain line needs one somewhere; set
     * this when the board does not provide an external resistor. */
    bool pull_up;

    /* Task priority and stack for the dispatcher. 0 selects the defaults, which
     * suit handlers that do a short I2C read. */
    uint32_t task_priority;
    uint32_t task_stack;
} int_dispatch_config_t;

/*
 * Configure the pin and start the dispatcher.
 *
 * Installs the GPIO ISR service if it is not already installed; a project that
 * installs it elsewhere is fine, as the duplicate is detected rather than
 * reported as an error.
 */
esp_err_t int_dispatch_start(const int_dispatch_config_t *cfg);
esp_err_t int_dispatch_stop(void);

/*
 * Register a handler. Called on every assertion of the line, in registration
 * order, because the line is shared and there is no way to know which part
 * asserted it without asking each one.
 *
 * ESP_ERR_NO_MEM if the table is full (CONFIG_INT_DISPATCH_MAX_HANDLERS).
 */
esp_err_t int_dispatch_register(int_dispatch_cb_t cb, void *ctx);

/*
 * How many times the line was still asserted after every handler had run.
 *
 * Worth watching: a non-zero and rising count means some part is asserting an
 * interrupt nobody clears, which turns a shared line into a busy loop. That is
 * otherwise invisible until the device feels slow.
 */
uint32_t int_dispatch_unserviced_count(void);

#ifdef __cplusplus
}
#endif

#endif /* INT_DISPATCH_H */
