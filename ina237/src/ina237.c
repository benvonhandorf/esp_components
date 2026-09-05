/*
 * TI INA237 current / voltage / power monitor.
 *
 * See include/ina237.h for the API and README.md for the datasheet quirks that
 * are not obvious from SBOSA20A.
 */
#include "ina237_priv.h"

#include <stdlib.h>

/*
 * Read a register.
 *
 * The INA237 needs the register pointer written before every read and does not
 * auto-increment across registers (section 7.5.1.1), so each value is its own
 * write-then-read transaction. Values arrive most significant byte first.
 */
static esp_err_t read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                          uint8_t *buffer, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buffer, len, XFER_TIMEOUT_MS);
}

static esp_err_t read_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t raw[2];
    esp_err_t err = read_reg(dev, reg, raw, sizeof(raw));
    if (err == ESP_OK) {
        *out = (uint16_t)((raw[0] << 8) | raw[1]);
    }
    return err;
}

static esp_err_t read_reg24(i2c_master_dev_handle_t dev, uint8_t reg, uint32_t *out)
{
    uint8_t raw[3];
    esp_err_t err = read_reg(dev, reg, raw, sizeof(raw));
    if (err == ESP_OK) {
        *out = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
    }
    return err;
}

static esp_err_t write_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value)
{
    const uint8_t payload[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_transmit(dev, payload, sizeof(payload), XFER_TIMEOUT_MS);
}

double ina237_current_lsb_for(double shunt_ohms)
{
    if (!(shunt_ohms > 0.0)) {
        return 0.0;
    }
    return VSHUNT_LSB_V / shunt_ohms;
}

esp_err_t ina237_create(const ina237_config_t *config, ina237_handle_t *out)
{
    if (!config || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    struct ina237_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    dev->dev = config->dev;
    /*
     * A shunt supplied here is remembered but not applied: nothing is
     * calibrated until ina237_configure() has proved the part is really an
     * INA237, and reporting a CURRENT_LSB for a part that never answered would
     * be a number with nothing behind it.
     */
    dev->shunt_ohms = config->shunt_ohms;

    *out = dev;
    return ESP_OK;
}

esp_err_t ina237_delete(ina237_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The I2C device handle belongs to the caller; it is not deleted here. */
    free(handle);
    return ESP_OK;
}

esp_err_t ina237_set_device(ina237_handle_t handle, i2c_master_dev_handle_t dev)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->dev = dev;
    return ESP_OK;
}

bool ina237_is_configured(ina237_handle_t handle)
{
    return handle && handle->configured;
}

double ina237_shunt_ohms(ina237_handle_t handle)
{
    return handle ? handle->shunt_ohms : 0.0;
}

double ina237_current_lsb(ina237_handle_t handle)
{
    return handle ? handle->current_lsb : 0.0;
}

double ina237_full_scale_amps(ina237_handle_t handle)
{
    if (!handle || !(handle->shunt_ohms > 0.0)) {
        return 0.0;
    }
    return INA237_SHUNT_FULL_SCALE_V / handle->shunt_ohms;
}

esp_err_t ina237_configure(ina237_handle_t handle, double shunt_ohms,
                           ina237_config_report_t *report)
{
    if (report) {
        *report = (ina237_config_report_t){0};
    }
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(shunt_ohms > 0.0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t manufacturer = 0;
    esp_err_t err = read_reg16(handle->dev, REG_MANUFACTURER_ID, &manufacturer);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = INA237_STAGE_PROBE;
        }
        return err;
    }

    if (manufacturer != INA237_MANUFACTURER_ID_TI) {
        if (report) {
            report->failed_stage = INA237_STAGE_IDENTIFY;
            report->manufacturer_id = manufacturer;
        }
        return ESP_ERR_INA237_WRONG_PART;
    }

    err = write_reg16(handle->dev, REG_SHUNT_CAL, INA237_SHUNT_CAL_VALUE);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = INA237_STAGE_SHUNT_CAL;
        }
        return err;
    }

    handle->shunt_ohms = shunt_ohms;
    handle->current_lsb = ina237_current_lsb_for(shunt_ohms);
    handle->configured = true;

    if (report) {
        report->manufacturer_id = manufacturer;
        report->current_lsb = handle->current_lsb;
        report->full_scale_amps = INA237_SHUNT_FULL_SCALE_V / shunt_ohms;
    }

    return ESP_OK;
}

esp_err_t ina237_read(ina237_handle_t handle, ina237_reading_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle->configured) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t vbus_raw = 0, vshunt_raw = 0, dietemp_raw = 0, current_raw = 0;
    uint16_t shunt_cal = 0, diag = 0;
    uint32_t power_raw = 0;
    esp_err_t err;

    if ((err = read_reg16(handle->dev, REG_VBUS, &vbus_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_VSHUNT, &vshunt_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_DIETEMP, &dietemp_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_CURRENT, &current_raw)) != ESP_OK ||
        (err = read_reg24(handle->dev, REG_POWER, &power_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_SHUNT_CAL, &shunt_cal)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_DIAG_ALRT, &diag)) != ESP_OK) {
        return err;
    }

    /* VBUS is unsigned; VSHUNT and CURRENT are two's complement; DIETEMP is a
     * signed 12-bit value living in bits 15:4, so it is sign-extended as 16
     * bits first and then shifted down. */
    out->bus_v = vbus_raw * VBUS_LSB_V;
    out->shunt_v = (int16_t)vshunt_raw * VSHUNT_LSB_V;
    out->temp_c = ((int16_t)dietemp_raw >> 4) * DIETEMP_LSB_C;
    out->current_a = (int16_t)current_raw * handle->current_lsb;
    out->power_w = power_raw * POWER_COEFFICIENT * handle->current_lsb;

    out->shunt_cal = shunt_cal;
    out->diag = diag;

    out->trim_checksum_ok = (diag & DIAG_MEMSTAT) != 0;
    out->math_overflow = (diag & DIAG_MATHOF) != 0;
    out->shunt_cal_matches = (shunt_cal == INA237_SHUNT_CAL_VALUE);

    return ESP_OK;
}

esp_err_t ina237_read_register(ina237_handle_t handle, uint8_t reg, uint16_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return read_reg16(handle->dev, reg, out);
}
