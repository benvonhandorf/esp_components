#ifndef FAKE_I2C_H
#define FAKE_I2C_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

typedef struct {
    uint16_t reg[0x40];
    uint8_t  power_high;      /* bits 23:16 of the POWER register */
    uint8_t  last_write_reg;
    unsigned writes;
    bool     present;         /* false makes every transfer fail */
} fake_ina237_t;

extern fake_ina237_t fake;

void fake_reset(void);

#endif
