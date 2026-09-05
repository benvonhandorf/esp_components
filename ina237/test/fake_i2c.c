/*
 * A fake INA237 on the far end of the I2C stub: a register map that answers
 * reads and records writes.
 *
 * The driver's decode path is the part worth testing -- sign extension, the
 * 24-bit power register, the LSB constants -- and none of it is reachable
 * without something to talk to.
 */
#include <string.h>

#include "fake_i2c.h"

fake_ina237_t fake;

void fake_reset(void)
{
    memset(&fake, 0, sizeof fake);
    /* Power-on defaults that matter here (Table 7-5, 7-6, 7-7, 7-20). */
    fake.reg[0x00] = 0x0000;   /* CONFIG: ADCRANGE = 0 */
    fake.reg[0x01] = 0xFB68;   /* ADC_CONFIG: continuous shunt, bus and temperature */
    fake.reg[0x02] = 0x1000;   /* SHUNT_CAL reset value, 4096 */
    fake.reg[0x0B] = 0x0001;   /* DIAG_ALRT: MEMSTAT = 1, trim checksum good */
    fake.reg[0x3E] = 0x5449;   /* MANUFACTURER_ID: "TI" */
    fake.present = true;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *write,
                              size_t write_len, int timeout_ms)
{
    (void)dev; (void)timeout_ms;
    if (!fake.present) {
        return ESP_FAIL;
    }
    if (write_len == 3) {
        fake.reg[write[0]] = (uint16_t)((write[1] << 8) | write[2]);
        fake.last_write_reg = write[0];
        fake.writes++;
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write,
                                      size_t write_len, uint8_t *read, size_t read_len,
                                      int timeout_ms)
{
    (void)dev; (void)write_len; (void)timeout_ms;
    if (!fake.present) {
        return ESP_FAIL;
    }
    const uint8_t reg = write[0];
    if (read_len == 2) {
        read[0] = (uint8_t)(fake.reg[reg] >> 8);
        read[1] = (uint8_t)fake.reg[reg];
    } else if (read_len == 3) {
        /* POWER is 24 bits; the fake keeps its low 16 in reg[] and the top 8
         * alongside, which is enough to exercise the assembly. */
        read[0] = fake.power_high;
        read[1] = (uint8_t)(fake.reg[reg] >> 8);
        read[2] = (uint8_t)fake.reg[reg];
    } else {
        return ESP_FAIL;
    }
    return ESP_OK;
}
