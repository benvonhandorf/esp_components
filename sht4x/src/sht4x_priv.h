/*
 * SHT4x command bytes, timings and handle state. Not on the include path --
 * this header is private to the component.
 */
#ifndef SHT4X_PRIV_H
#define SHT4X_PRIV_H

#include "sht4x.h"

/* Table 8, Overview of I2C commands. */
#define CMD_READ_SERIAL 0x89
#define CMD_SOFT_RESET  0x94

/*
 * Table 5, System timing: the measurement takes at most 8.3 ms even at high
 * repeatability, so one value covers all three modes. See wait_ms() for why
 * this cannot simply be handed to pdMS_TO_TICKS().
 */
#define MEASURE_WAIT_MS 10
#define SERIAL_WAIT_MS  10
#define RESET_WAIT_MS   10

/* Heater pulses are 1.1 s / 0.11 s max, plus the measurement that follows. */
#define HEATER_LONG_WAIT_MS  1200
#define HEATER_SHORT_WAIT_MS 200

/* Every reply is two 16-bit words, each followed by its own CRC. */
#define RESPONSE_LEN 6

#define XFER_TIMEOUT_MS 1000

struct sht4x_dev_t {
    i2c_master_dev_handle_t dev;
};

#endif
