/* Host stub: see ../esp_err.h. Just enough of ESP-IDF's spi_master.h to build
 * the driver off-target -- the handle type and the one transfer call it uses. */
#ifndef STUB_SPI_MASTER_H
#define STUB_SPI_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct spi_device_t *spi_device_handle_t;

typedef struct {
    uint8_t mode;
    int clock_speed_hz;
    int spics_io_num;
    int queue_size;
} spi_device_interface_config_t;

typedef struct {
    size_t length; /* in bits */
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;

esp_err_t spi_device_polling_transmit(spi_device_handle_t dev, spi_transaction_t *t);

#endif
