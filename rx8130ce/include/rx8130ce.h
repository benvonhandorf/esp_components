#ifndef RX8130CE_H
#define RX8130CE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Epson RX8130CE real-time clock with battery backup.
 *
 * Returns facts rather than formatted text, so the caller decides how to report
 * them.
 */

#define ESP_ERR_RX8130CE_BASE       0x3A000
/* The registers do not decode to a valid date. Usually a part whose backup has
 * drained, which is worth distinguishing from an I2C failure: the part is there
 * and answering, it just does not know what time it is. */
#define ESP_ERR_RX8130CE_NOT_SET    (ESP_ERR_RX8130CE_BASE + 1)
/* The time is outside the 2000-2099 the part can represent. */
#define ESP_ERR_RX8130CE_BAD_TIME   (ESP_ERR_RX8130CE_BASE + 2)

#define RX8130CE_I2C_ADDR_DEFAULT 0x32

typedef struct rx8130ce_dev_t *rx8130ce_handle_t;

typedef struct {
    /* May be NULL; calls needing the bus return ESP_ERR_INVALID_STATE until
     * rx8130ce_set_device() supplies one. The caller owns it. */
    i2c_master_dev_handle_t dev;
} rx8130ce_config_t;

typedef struct {
    /* Whether the part reported losing power since it was last set. A clock that
     * answers but has been unpowered holds a plausible-looking wrong time, so
     * this is the difference between "the RTC says 03:00" and "the RTC is
     * telling you it does not know". */
    bool power_lost;
    uint8_t flag_register;
} rx8130ce_report_t;

esp_err_t rx8130ce_create(const rx8130ce_config_t *cfg, rx8130ce_handle_t *out,
                          rx8130ce_report_t *report);
void      rx8130ce_delete(rx8130ce_handle_t handle);
esp_err_t rx8130ce_set_device(rx8130ce_handle_t handle, i2c_master_dev_handle_t dev,
                              rx8130ce_report_t *report);

/* ESP_ERR_RX8130CE_NOT_SET if the registers do not decode to a valid date. */
esp_err_t rx8130ce_get_time(rx8130ce_handle_t handle, struct timeval *out);

/* ESP_ERR_RX8130CE_BAD_TIME outside 2000-2099. Clears the power-lost flag. */
esp_err_t rx8130ce_set_time(rx8130ce_handle_t handle, const struct timeval *tv);

/* Whether the part has reported losing power since it was last set. */
bool rx8130ce_power_was_lost(rx8130ce_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* RX8130CE_H */
