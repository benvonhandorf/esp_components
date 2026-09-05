/*
 * INA237 register map and handle state. Not on the include path -- this header
 * is private to the component.
 */
#ifndef INA237_PRIV_H
#define INA237_PRIV_H

#include "ina237.h"

/* Table 7-3, INA237 Registers. Note there is no DEVICE_ID register: the map
 * ends at MANUFACTURER_ID, unlike the otherwise similar INA238/INA228. */
#define REG_CONFIG          0x00
#define REG_ADC_CONFIG      0x01
#define REG_SHUNT_CAL       0x02
#define REG_VSHUNT          0x04
#define REG_VBUS            0x05
#define REG_DIETEMP         0x06
#define REG_CURRENT         0x07
#define REG_POWER           0x08  /* 24-bit */
#define REG_DIAG_ALRT       0x0B
#define REG_MANUFACTURER_ID 0x3E

/* Table 7-19, DIAG_ALRT register. */
#define DIAG_MATHOF  (1u << 9)
#define DIAG_CNVRF   (1u << 1)
#define DIAG_MEMSTAT (1u << 0) /* 1 = normal, 0 = trim checksum error */

/* Table 8-1, ADC Full Scale Values, at ADCRANGE = 0. */
#define VSHUNT_LSB_V   5.0e-6
#define VBUS_LSB_V     3.125e-3
#define DIETEMP_LSB_C  0.125

/* Equation 4: Power [W] = 0.2 x CURRENT_LSB x POWER. */
#define POWER_COEFFICIENT 0.2

#define XFER_TIMEOUT_MS 1000

struct ina237_dev_t {
    i2c_master_dev_handle_t dev;
    bool configured;
    double shunt_ohms;
    double current_lsb; /* amperes per CURRENT register count */
};

#endif
