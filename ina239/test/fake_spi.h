#ifndef FAKE_SPI_H
#define FAKE_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"

/*
 * A register file behind an SPI frame decoder.
 *
 * It decodes the command byte the way the datasheet says the part does --
 * address in bits 7:2, R/W in bit 0 -- rather than the way the driver happens
 * to encode it, so a driver that put the address in the wrong place, or the
 * read bit at the wrong end, fails here instead of on a bench.
 */
typedef struct {
    uint16_t reg[0x40];
    uint8_t power_low;      /* bits 7:0 of the 24-bit POWER register */
    uint8_t last_write_reg;
    unsigned writes;
    unsigned reads;
    /* The last frame seen, so a test can assert on its exact shape. */
    uint8_t last_frame[8];
    size_t last_frame_len;
    bool present;           /* false makes every transfer fail */
} fake_ina239_t;

extern fake_ina239_t fake;

void fake_reset(void);

#endif
