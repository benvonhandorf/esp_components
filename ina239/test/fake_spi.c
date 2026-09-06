/*
 * A fake INA239 on the far side of spi_device_polling_transmit().
 *
 * The point of decoding the frame here rather than trusting the driver's own
 * CMD_READ/CMD_WRITE macros is that those macros are exactly what is under
 * test: an address shifted by one bit too few, or a read bit written at bit 1
 * instead of bit 0, still round-trips through a fake that reuses them.
 */
#include "fake_spi.h"

#include <string.h>

#include "esp_err.h"

fake_ina239_t fake;

void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.present = true;
    /* The datasheet's reset values for the registers a test is likely to look
     * at without having written them first (Table 7-3). */
    fake.reg[0x01] = 0xFB68; /* ADC_CONFIG */
    fake.reg[0x02] = 0x1000; /* SHUNT_CAL  */
    fake.reg[0x0B] = 0x0001; /* DIAG_ALRT, MEMSTAT set = trim memory good */
    fake.reg[0x3E] = 0x5449; /* MANUFACTURER_ID, "TI" */
    fake.reg[0x3F] = 0x2391; /* DEVICE_ID, die 239h revision 1 */
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t dev, spi_transaction_t *t)
{
    (void)dev;
    if (!fake.present) {
        return ESP_FAIL;
    }
    if (!t || t->length < 8 || !t->tx_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *tx = (const uint8_t *)t->tx_buffer;
    const size_t len = t->length / 8;

    fake.last_frame_len = len < sizeof(fake.last_frame) ? len : sizeof(fake.last_frame);
    memcpy(fake.last_frame, tx, fake.last_frame_len);

    /* Table 7-2: ADDR5..ADDR0 in bits 7:2, bit 1 always 0, bit 0 is R/W. */
    const uint8_t cmd = tx[0];
    if (cmd & 0x02) {
        return ESP_FAIL; /* the reserved bit must be low */
    }
    const uint8_t reg = (uint8_t)((cmd >> 2) & 0x3F);
    const bool read = (cmd & 0x01) != 0;

    if (read) {
        if (!t->rx_buffer) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t *rx = (uint8_t *)t->rx_buffer;
        memset(rx, 0, len);
        /* MISO is held low for the command byte, so the data starts at rx[1]. */
        if (len >= 3) {
            rx[1] = (uint8_t)(fake.reg[reg] >> 8);
            rx[2] = (uint8_t)(fake.reg[reg] & 0xFF);
        }
        if (len >= 4) {
            /* Only POWER is 24 bits wide; everything else would read its low
             * byte as zero, which is what a real part does too. */
            rx[3] = (reg == 0x08) ? fake.power_low : 0;
        }
        fake.reads++;
        return ESP_OK;
    }

    if (len != 3) {
        return ESP_FAIL; /* every writable register is 16 bits (7.5.1.1) */
    }
    fake.reg[reg] = (uint16_t)((tx[1] << 8) | tx[2]);
    fake.last_write_reg = reg;
    fake.writes++;
    return ESP_OK;
}
