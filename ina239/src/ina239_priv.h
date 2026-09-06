/*
 * INA239 register map and handle state. Not on the include path -- this header
 * is private to the component.
 */
#ifndef INA239_PRIV_H
#define INA239_PRIV_H

#include "ina239.h"

/* Table 7-3, INA239 Registers. */
#define REG_CONFIG          0x00
#define REG_ADC_CONFIG      0x01
#define REG_SHUNT_CAL       0x02
#define REG_VSHUNT          0x04
#define REG_VBUS            0x05
#define REG_DIETEMP         0x06
#define REG_CURRENT         0x07
#define REG_POWER           0x08 /* 24-bit */
#define REG_DIAG_ALRT       0x0B
#define REG_SOVL            0x0C
#define REG_SUVL            0x0D
#define REG_BOVL            0x0E
#define REG_BUVL            0x0F
#define REG_TEMP_LIMIT      0x10
#define REG_PWR_LIMIT       0x11
#define REG_MANUFACTURER_ID 0x3E
#define REG_DEVICE_ID       0x3F

/*
 * Table 7-2, first 8 MSB bits of an SPI frame: the six address bits sit in
 * 7:2, bit 1 is always 0, and bit 0 is R/W -- 1 reads, 0 writes.
 */
#define CMD_READ(reg)  (uint8_t)(((reg) << 2) | 0x01)
#define CMD_WRITE(reg) (uint8_t)((reg) << 2)

/* Table 7-5, CONFIG register. */
#define CONFIG_RST      (1u << 15)
#define CONFIG_ADCRANGE (1u << 4)

/*
 * Table 7-6, ADC_CONFIG. MODE Fh is continuous bus voltage, shunt voltage and
 * temperature -- everything ina239_read() reports, so anything less would leave
 * one of the five values frozen at whatever it last held.
 */
#define ADC_CONFIG_MODE_CONTINUOUS_ALL 0xF
#define ADC_CONFIG_MODE_SHIFT          12
/*
 * VBUSCT, VSHCT and VTCT, 3 bits each, left at their reset encoding of 5h
 * (1052 us). The conversion time trades noise against how fast the averaged
 * result lands, and 1052 us is the datasheet's own default for all three.
 */
#define ADC_CONFIG_CT_DEFAULT 0x5
#define ADC_CONFIG_VBUSCT_SHIFT 9
#define ADC_CONFIG_VSHCT_SHIFT  6
#define ADC_CONFIG_VTCT_SHIFT   3

/* Table 7-13, DIAG_ALRT register. */
#define DIAG_MATHOF  (1u << 9)
#define DIAG_CNVRF   (1u << 1)
#define DIAG_MEMSTAT (1u << 0) /* 1 = normal, 0 = trim checksum error */

/* Tables 7-8 through 7-10, and section 8.1.2. */
#define VSHUNT_LSB_V_LOW  5.0e-6  /* ADCRANGE = 0 */
#define VSHUNT_LSB_V_HIGH 1.25e-6 /* ADCRANGE = 1 */
#define VBUS_LSB_V        3.125e-3
#define DIETEMP_LSB_C     0.125 /* the value is 12 bits, in DIETEMP[15:4] */

/* Equation 4: Power [W] = 0.2 x CURRENT_LSB x POWER. */
#define POWER_COEFFICIENT 0.2

/* Section 7.6.1.1: RST self-clears, but the part still needs its power-on time
 * before it answers again. */
#define RESET_SETTLE_MS 2

struct ina239_dev_t {
    spi_device_handle_t dev;
    bool configured;
    double shunt_ohms;
    double current_lsb; /* amperes per CURRENT register count */
    ina239_range_t range;
    ina239_averaging_t averaging;
    /* What configure() wrote, so a later read can tell whether the part still
     * holds it. Kept rather than recomputed: the comparison must be against the
     * bytes actually sent, not against a second derivation that could drift. */
    uint16_t config_written;
    uint16_t adc_config_written;
};

/* The shunt LSB for a range, in volts. 0 for an invalid encoding. */
double ina239_shunt_lsb_v(ina239_range_t range);

#endif /* INA239_PRIV_H */
