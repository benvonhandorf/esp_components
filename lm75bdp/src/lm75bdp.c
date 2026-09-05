#include "lm75bdp.h"

#include <stdlib.h>
#include <string.h>

#include "lm75bdp_calc.h"

#define REG_TEMP  0x00
#define REG_CONF  0x01
#define REG_THYST 0x02
#define REG_TOS   0x03

/* Comparator mode, OS active low, one fault before the output asserts. A single
 * fault responds quickly; raise it if the OS line is used to cut power and a
 * transient must not trip it. */
#define CONF_DEFAULT 0x00

#define I2C_TIMEOUT_MS 1000

struct lm75bdp_dev_t {
    i2c_master_dev_handle_t dev;
};

const char *lm75bdp_stage_name(lm75bdp_stage_t stage)
{
    switch (stage) {
        case LM75BDP_STAGE_NONE:      return "none";
        case LM75BDP_STAGE_IDENTIFY:  return "confirming the part responds";
        case LM75BDP_STAGE_CONFIGURE: return "configuring";
        case LM75BDP_STAGE_THRESHOLD: return "writing a threshold";
    }
    return "unknown";
}

static esp_err_t write8(struct lm75bdp_dev_t *d, uint8_t reg, uint8_t value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t write16(struct lm75bdp_dev_t *d, uint8_t reg, uint16_t value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t read8(struct lm75bdp_dev_t *d, uint8_t reg, uint8_t *value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(d->dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t read16(struct lm75bdp_dev_t *d, uint8_t reg, uint16_t *value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2];
    esp_err_t err = i2c_master_transmit_receive(d->dev, &reg, 1, buf, sizeof(buf),
                                                I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *value = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

static esp_err_t configure(struct lm75bdp_dev_t *d, lm75bdp_report_t *report)
{
    if (report) report->failed_stage = LM75BDP_STAGE_NONE;
    if (!d->dev) return ESP_ERR_INVALID_STATE;

    esp_err_t err = write8(d, REG_CONF, CONF_DEFAULT);
    if (err != ESP_OK) {
        if (report) report->failed_stage = LM75BDP_STAGE_CONFIGURE;
        return err;
    }

    /*
     * There is no ID register, so confirm the part by reading the configuration
     * back. The LM75B leaves its reserved bits clear, so an address that
     * acknowledges but is something else will not reproduce the write.
     */
    uint8_t readback = 0xFF;
    err = read8(d, REG_CONF, &readback);
    if (err != ESP_OK) {
        if (report) report->failed_stage = LM75BDP_STAGE_IDENTIFY;
        return err;
    }
    if (readback != CONF_DEFAULT) {
        if (report) report->failed_stage = LM75BDP_STAGE_IDENTIFY;
        return ESP_ERR_LM75BDP_WRONG_PART;
    }

    return ESP_OK;
}

esp_err_t lm75bdp_create(const lm75bdp_config_t *cfg, lm75bdp_handle_t *out,
                         lm75bdp_report_t *report)
{
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (report) memset(report, 0, sizeof(*report));

    struct lm75bdp_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->dev = cfg->dev;

    if (d->dev) {
        esp_err_t err = configure(d, report);
        if (err != ESP_OK) {
            free(d);
            return err;
        }
    }

    *out = d;
    return ESP_OK;
}

void lm75bdp_delete(lm75bdp_handle_t handle) { free(handle); }

esp_err_t lm75bdp_set_device(lm75bdp_handle_t handle, i2c_master_dev_handle_t dev,
                             lm75bdp_report_t *report)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (report) memset(report, 0, sizeof(*report));
    handle->dev = dev;
    return dev ? configure(handle, report) : ESP_OK;
}

esp_err_t lm75bdp_read(lm75bdp_handle_t handle, lm75bdp_reading_t *out)
{
    if (!handle || !out) return ESP_ERR_INVALID_ARG;

    uint16_t raw = 0;
    esp_err_t err = read16(handle, REG_TEMP, &raw);
    if (err != ESP_OK) return err;

    out->temperature_C = lm75bdp_temperature_c(raw);
    return ESP_OK;
}

esp_err_t lm75bdp_set_thresholds(lm75bdp_handle_t handle, float tos_C, float thyst_C,
                                 lm75bdp_report_t *report)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (report) memset(report, 0, sizeof(*report));

    const uint16_t tos = lm75bdp_encode_threshold(tos_C);
    const uint16_t thyst = lm75bdp_encode_threshold(thyst_C);

    /* Report what was actually programmed: both are quantised to 0.5 C and
     * clamped, so they are often not what was asked for. */
    if (report) {
        report->tos_C = lm75bdp_decode_threshold(tos);
        report->thyst_C = lm75bdp_decode_threshold(thyst);
    }

    esp_err_t err = write16(handle, REG_TOS, tos);
    if (err == ESP_OK) err = write16(handle, REG_THYST, thyst);
    if (err != ESP_OK && report) {
        report->failed_stage = LM75BDP_STAGE_THRESHOLD;
    }
    return err;
}
