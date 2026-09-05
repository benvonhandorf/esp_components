/* Host stub: see ../esp_err.h. */
#ifndef STUB_I2C_MASTER_H
#define STUB_I2C_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *write,
                              size_t write_len, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write,
                                      size_t write_len, uint8_t *read, size_t read_len,
                                      int timeout_ms);

#endif
