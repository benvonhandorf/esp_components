#include "ina219.h"

#include <stdlib.h>
#include <string.h>

#include "ina219_calc.h"

/* Registers */
#define REG_CONFIG        0x00
#define REG_SHUNT_VOLTAGE 0x01
#define REG_BUS_VOLTAGE   0x02
#define REG_POWER         0x03
#define REG_CURRENT       0x04
#define REG_CALIBRATION   0x05

#define CONFIG_RESET      0x8000
/* 32 V bus range, PGA /8 (+/-320 mV), 12-bit ADCs, continuous shunt and bus.
 * The reference design's settings, and reasonable defaults. */
#define CONFIG_BVOLT_32V  0x2000
#define CONFIG_GAIN_8     0x1800
#define CONFIG_BADC_12BIT 0x0180
#define CONFIG_SADC_12BIT 0x0018
#define CONFIG_MODE_CONT  0x0007

#define I2C_TIMEOUT_MS 1000

struct ina219_dev_t {
    i2c_master_dev_handle_t dev;
    ina219_config_t cfg;
    ina219_calibration_t cal;
};

const char *ina219_stage_name(ina219_stage_t stage)
{
    switch (stage) {
        case INA219_STAGE_NONE:      return "none";
        case INA219_STAGE_RANGE:     return "computing the calibration";
        case INA219_STAGE_IDENTIFY:  return "confirming the part responds";
        case INA219_STAGE_RESET:     return "resetting";
        case INA219_STAGE_CONFIGURE: return "configuring";
        case INA219_STAGE_CALIBRATE: return "writing the calibration";
    }
    return "unknown";
}

static esp_err_t write_reg(struct ina219_dev_t *d, uint8_t reg, uint16_t value)
{
    if (!d->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t read_reg(struct ina219_dev_t *d, uint8_t reg, uint16_t *value)
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

static esp_err_t fail(ina219_report_t *report, ina219_stage_t stage, esp_err_t err)
{
    if (report) {
        report->failed_stage = stage;
    }
    return err;
}

static esp_err_t configure(struct ina219_dev_t *d, ina219_report_t *report)
{
    if (report) {
        report->failed_stage = INA219_STAGE_NONE;
        report->current_lsb_a = d->cal.current_lsb_a;
        report->full_scale_a = d->cal.full_scale_a;
        report->calibration = d->cal.calibration;
    }

    if (!d->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = write_reg(d, REG_CONFIG, CONFIG_RESET);
    if (err != ESP_OK) {
        return fail(report, INA219_STAGE_RESET, err);
    }

    const uint16_t config = CONFIG_BVOLT_32V | CONFIG_GAIN_8 | CONFIG_BADC_12BIT |
                            CONFIG_SADC_12BIT | CONFIG_MODE_CONT;
    err = write_reg(d, REG_CONFIG, config);
    if (err != ESP_OK) {
        return fail(report, INA219_STAGE_CONFIGURE, err);
    }

    err = write_reg(d, REG_CALIBRATION, d->cal.calibration);
    if (err != ESP_OK) {
        return fail(report, INA219_STAGE_CALIBRATE, err);
    }

    /*
     * The INA219 has no ID registers, so confirm it is really there by reading
     * back what was just written. An address that acknowledges but is something
     * else -- or a bus with pull-ups and no part -- will not reproduce it, and
     * silently returning zeros forever is the alternative.
     */
    uint16_t readback = 0;
    err = read_reg(d, REG_CALIBRATION, &readback);
    if (err != ESP_OK) {
        return fail(report, INA219_STAGE_IDENTIFY, err);
    }
    if (readback != d->cal.calibration) {
        return fail(report, INA219_STAGE_IDENTIFY, ESP_ERR_INA219_WRONG_PART);
    }

    return ESP_OK;
}

esp_err_t ina219_create(const ina219_config_t *cfg, ina219_handle_t *out,
                        ina219_report_t *report)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (report) {
        memset(report, 0, sizeof(*report));
    }

    struct ina219_dev_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }

    d->cfg = *cfg;
    d->dev = cfg->dev;

    if (!ina219_calibration_compute(cfg->shunt_ohms, cfg->max_current_a, &d->cal)) {
        free(d);
        return fail(report, INA219_STAGE_RANGE, ESP_ERR_INA219_BAD_RANGE);
    }

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

void ina219_delete(ina219_handle_t handle)
{
    free(handle);
}

esp_err_t ina219_set_device(ina219_handle_t handle, i2c_master_dev_handle_t dev,
                            ina219_report_t *report)
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

esp_err_t ina219_read(ina219_handle_t handle, ina219_reading_t *out)
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

    out->shunt_voltage = ina219_shunt_volts(shunt);
    out->bus_voltage   = ina219_bus_volts(bus);
    /* Scaled by the LSB the calibration register actually yields. Scaling by the
     * requested LSB instead is what made the original read 24.5% high. */
    out->current       = ina219_current_amps(current, handle->cal.current_lsb_a);
    out->power         = ina219_power_watts(power, handle->cal.power_lsb_w);
    return ESP_OK;
}

esp_err_t ina219_clear_alert(ina219_handle_t handle)
{
    /* The INA219 has no alert latch to clear; provided so callers can treat the
     * current monitors uniformly. */
    return handle ? ESP_OK : ESP_ERR_INVALID_ARG;
}
