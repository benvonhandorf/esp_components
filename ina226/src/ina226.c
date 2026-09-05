#include "ina226.h"

#include <stdlib.h>
#include <string.h>

#include "ina226_calc.h"

/* Registers */
#define REG_CONFIG          0x00
#define REG_SHUNT_VOLTAGE   0x01
#define REG_BUS_VOLTAGE     0x02
#define REG_POWER           0x03
#define REG_CURRENT         0x04
#define REG_CALIBRATION     0x05
#define REG_MASK_ENABLE     0x06
#define REG_MANUFACTURER_ID 0xFE
#define REG_DIE_ID          0xFF

#define CONFIG_RESET        0x8000
#define CONFIG_MODE_BOTH_CONT 0x0007
/* 1.1 ms conversion for both bus and shunt: the reference design's choice, and a
 * reasonable default -- long enough to reject mains hum, short enough to react. */
#define CONFIG_VBUS_1100US    0x0100
#define CONFIG_VSHUNT_1100US  0x0020

#define I2C_TIMEOUT_MS 1000

struct ina226_dev_t {
    i2c_master_dev_handle_t dev;
    ina226_config_t cfg;
    ina226_calibration_t cal;
};

const char *ina226_stage_name(ina226_stage_t stage)
{
    switch (stage) {
        case INA226_STAGE_NONE:      return "none";
        case INA226_STAGE_RANGE:     return "computing the calibration";
        case INA226_STAGE_IDENTIFY:  return "identifying the part";
        case INA226_STAGE_RESET:     return "resetting";
        case INA226_STAGE_CONFIGURE: return "configuring";
        case INA226_STAGE_CALIBRATE: return "writing the calibration";
    }
    return "unknown";
}

static esp_err_t write_reg(struct ina226_dev_t *d, uint8_t reg, uint16_t value)
{
    if (!d->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t read_reg(struct ina226_dev_t *d, uint8_t reg, uint16_t *value)
{
    if (!d->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2];
    esp_err_t err = i2c_master_transmit_receive(d->dev, &reg, 1, buf, sizeof(buf),
                                                I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *value = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

static esp_err_t fail(ina226_report_t *report, ina226_stage_t stage, esp_err_t err)
{
    if (report) {
        report->failed_stage = stage;
    }
    return err;
}

/* Configure the part: identify it, reset, set averaging and timing, calibrate. */
static esp_err_t configure(struct ina226_dev_t *d, ina226_report_t *report)
{
    if (report) {
        report->failed_stage = INA226_STAGE_NONE;
        report->current_lsb_a = d->cal.current_lsb_a;
        report->full_scale_a = d->cal.full_scale_a;
        report->calibration = d->cal.calibration;
    }

    if (!d->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Identify before touching anything. Writing a reset to whatever happens to
     * answer at 0x40 -- an INA219, an EEPROM -- is worse than reporting that the
     * part is not what was expected.
     */
    uint16_t manufacturer = 0;
    uint16_t die = 0;
    esp_err_t err = read_reg(d, REG_MANUFACTURER_ID, &manufacturer);
    if (err == ESP_OK) {
        err = read_reg(d, REG_DIE_ID, &die);
    }
    if (err != ESP_OK) {
        return fail(report, INA226_STAGE_IDENTIFY, err);
    }
    if (report) {
        report->manufacturer_id = manufacturer;
        report->die_id = die;
    }
    /*
     * Compare the device half of the die ID only. The low nibble is the die
     * revision, and the register map lists 2260h and 2261h as the same part, so
     * requiring all sixteen bits rejects genuine INA226s.
     */
    if (!ina226_part_matches(manufacturer, die)) {
        return fail(report, INA226_STAGE_IDENTIFY, ESP_ERR_INA226_WRONG_PART);
    }

    err = write_reg(d, REG_CONFIG, CONFIG_RESET);
    if (err != ESP_OK) {
        return fail(report, INA226_STAGE_RESET, err);
    }

    const uint16_t config = (uint16_t)((uint16_t)d->cfg.averaging << 9) |
                            CONFIG_VBUS_1100US | CONFIG_VSHUNT_1100US |
                            CONFIG_MODE_BOTH_CONT;
    err = write_reg(d, REG_CONFIG, config);
    if (err != ESP_OK) {
        return fail(report, INA226_STAGE_CONFIGURE, err);
    }

    err = write_reg(d, REG_CALIBRATION, d->cal.calibration);
    if (err != ESP_OK) {
        return fail(report, INA226_STAGE_CALIBRATE, err);
    }

    return ESP_OK;
}

esp_err_t ina226_create(const ina226_config_t *cfg, ina226_handle_t *out,
                        ina226_report_t *report)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (report) {
        memset(report, 0, sizeof(*report));
    }

    struct ina226_dev_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }

    d->cfg = *cfg;
    d->dev = cfg->dev;

    if (!ina226_calibration_compute(cfg->shunt_ohms, cfg->max_current_a, &d->cal)) {
        free(d);
        return fail(report, INA226_STAGE_RANGE, ESP_ERR_INA226_BAD_RANGE);
    }

    /*
     * A handle is usable before there is a bus: a caller may want to state how
     * the board is wired before anything is powered. Configuration happens now if
     * a device was supplied, and in set_device() otherwise.
     */
    if (d->dev) {
        esp_err_t err = configure(d, report);
        if (err != ESP_OK) {
            free(d);
            return err;
        }
    } else if (report) {
        report->current_lsb_a = d->cal.current_lsb_a;
        report->full_scale_a = d->cal.full_scale_a;
        report->calibration = d->cal.calibration;
    }

    *out = d;
    return ESP_OK;
}

void ina226_delete(ina226_handle_t handle)
{
    /* The I2C device belongs to the caller; deleting it here would invalidate a
     * handle they may still be using for something else at the same address. */
    free(handle);
}

esp_err_t ina226_set_device(ina226_handle_t handle, i2c_master_dev_handle_t dev,
                            ina226_report_t *report)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    handle->dev = dev;
    if (!dev) {
        return ESP_OK;
    }
    return configure(handle, report);
}

esp_err_t ina226_read(ina226_handle_t handle, ina226_reading_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t shunt = 0, bus = 0, power = 0, current = 0;

    esp_err_t err = read_reg(handle, REG_SHUNT_VOLTAGE, &shunt);
    if (err == ESP_OK) err = read_reg(handle, REG_BUS_VOLTAGE, &bus);
    if (err == ESP_OK) err = read_reg(handle, REG_POWER, &power);
    if (err == ESP_OK) err = read_reg(handle, REG_CURRENT, &current);
    if (err != ESP_OK) {
        return err;
    }

    out->shunt_voltage = ina226_shunt_volts(shunt);
    out->bus_voltage   = ina226_bus_volts(bus);
    out->current       = ina226_current_amps(current, handle->cal.current_lsb_a);
    out->power         = ina226_power_watts(power, handle->cal.power_lsb_w);
    return ESP_OK;
}

esp_err_t ina226_clear_alert(ina226_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Reading mask/enable clears AFF, CVRF and OVF. The value is not otherwise
     * interesting here, so it is read and discarded. */
    uint16_t mask_enable;
    return read_reg(handle, REG_MASK_ENABLE, &mask_enable);
}
