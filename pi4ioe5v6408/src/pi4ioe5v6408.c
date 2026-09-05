#include "pi4ioe5v6408.h"

#include <stdlib.h>
#include <string.h>

#define REG_CONTROL       0x01
#define REG_DIRECTION     0x03   /* 1 = output */
#define REG_OUTPUT        0x05
#define REG_OUTPUT_HI_Z   0x07   /* 1 = high impedance */
#define REG_INPUT_DEFAULT 0x09
#define REG_PULL_ENABLE   0x0B
#define REG_PULL_SELECT   0x0D   /* 1 = pull-up */
#define REG_INPUT         0x0F
#define REG_INT_MASK      0x11   /* 1 = masked */
#define REG_INT_STATUS    0x13

/* The control register's upper bits carry a fixed device ID. */
#define CONTROL_DEVICE_ID_MASK 0xFC
#define CONTROL_DEVICE_ID      0xA0

#define I2C_TIMEOUT_MS 1000

struct pi4ioe5v6408_dev_t {
    i2c_master_dev_handle_t dev;
    pi4ioe5v6408_config_t cfg;
    /* The output register cannot be read back meaningfully once pins are inputs,
     * so the last written value is kept here for read-modify-write. */
    uint8_t out;
};

const char *pi4ioe5v6408_stage_name(pi4ioe5v6408_stage_t stage)
{
    switch (stage) {
        case PI4IOE5V6408_STAGE_NONE:      return "none";
        case PI4IOE5V6408_STAGE_IDENTIFY:  return "identifying the part";
        case PI4IOE5V6408_STAGE_PRESET:    return "establishing output levels";
        case PI4IOE5V6408_STAGE_CONFIGURE: return "configuring";
    }
    return "unknown";
}

static esp_err_t write8(struct pi4ioe5v6408_dev_t *d, uint8_t reg, uint8_t value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t read8(struct pi4ioe5v6408_dev_t *d, uint8_t reg, uint8_t *value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(d->dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t fail(pi4ioe5v6408_report_t *r, pi4ioe5v6408_stage_t s, esp_err_t e)
{
    if (r) r->failed_stage = s;
    return e;
}

static esp_err_t configure(struct pi4ioe5v6408_dev_t *d, pi4ioe5v6408_report_t *report)
{
    if (report) report->failed_stage = PI4IOE5V6408_STAGE_NONE;
    if (!d->dev) return ESP_ERR_INVALID_STATE;

    uint8_t control = 0;
    esp_err_t err = read8(d, REG_CONTROL, &control);
    if (err != ESP_OK) return fail(report, PI4IOE5V6408_STAGE_IDENTIFY, err);
    if (report) report->control_register = control;
    if ((control & CONTROL_DEVICE_ID_MASK) != CONTROL_DEVICE_ID) {
        return fail(report, PI4IOE5V6408_STAGE_IDENTIFY, ESP_ERR_PI4IOE5V6408_WRONG_PART);
    }

    /* Levels before direction, so a pin does not briefly drive whatever the
     * register held from a previous run. */
    d->out = d->cfg.initial;
    err = write8(d, REG_OUTPUT, d->out);
    if (err != ESP_OK) return fail(report, PI4IOE5V6408_STAGE_PRESET, err);

    /* Take the output pins out of high impedance; leave the inputs in it. */
    err = write8(d, REG_OUTPUT_HI_Z, (uint8_t)~d->cfg.outputs);
    if (err == ESP_OK) err = write8(d, REG_DIRECTION, d->cfg.outputs);
    if (err == ESP_OK) err = write8(d, REG_PULL_ENABLE, d->cfg.pull_enable);
    if (err == ESP_OK) err = write8(d, REG_PULL_SELECT, d->cfg.pull_up);
    /* The interrupt mask is inverted: a 1 bit masks the pin off. */
    if (err == ESP_OK) err = write8(d, REG_INT_MASK, (uint8_t)~d->cfg.interrupt_on);
    if (err != ESP_OK) return fail(report, PI4IOE5V6408_STAGE_CONFIGURE, err);

    return ESP_OK;
}

esp_err_t pi4ioe5v6408_create(const pi4ioe5v6408_config_t *cfg,
                              pi4ioe5v6408_handle_t *out,
                              pi4ioe5v6408_report_t *report)
{
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (report) memset(report, 0, sizeof(*report));

    struct pi4ioe5v6408_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->cfg = *cfg;
    d->dev = cfg->dev;
    d->out = cfg->initial;

    if (d->dev) {
        esp_err_t err = configure(d, report);
        if (err != ESP_OK) { free(d); return err; }
    }

    *out = d;
    return ESP_OK;
}

void pi4ioe5v6408_delete(pi4ioe5v6408_handle_t handle) { free(handle); }

esp_err_t pi4ioe5v6408_set_device(pi4ioe5v6408_handle_t handle,
                                  i2c_master_dev_handle_t dev,
                                  pi4ioe5v6408_report_t *report)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (report) memset(report, 0, sizeof(*report));
    handle->dev = dev;
    return dev ? configure(handle, report) : ESP_OK;
}

esp_err_t pi4ioe5v6408_write_port(pi4ioe5v6408_handle_t handle, uint8_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    esp_err_t err = write8(handle, REG_OUTPUT, value);
    if (err == ESP_OK) handle->out = value;
    return err;
}

esp_err_t pi4ioe5v6408_read_port(pi4ioe5v6408_handle_t handle, uint8_t *value)
{
    if (!handle || !value) return ESP_ERR_INVALID_ARG;
    return read8(handle, REG_INPUT, value);
}

esp_err_t pi4ioe5v6408_set_pin(pi4ioe5v6408_handle_t handle, uint8_t pin, bool high)
{
    if (!handle || pin > 7) return ESP_ERR_INVALID_ARG;

    /* From the shadow: the input register reflects pin state, not what was last
     * driven, so a read-modify-write through it would write input levels back
     * over the outputs. */
    uint8_t value = handle->out;
    if (high) {
        value |= (uint8_t)(1u << pin);
    } else {
        value &= (uint8_t)~(1u << pin);
    }
    return pi4ioe5v6408_write_port(handle, value);
}

esp_err_t pi4ioe5v6408_get_pin(pi4ioe5v6408_handle_t handle, uint8_t pin, bool *high)
{
    if (!handle || !high || pin > 7) return ESP_ERR_INVALID_ARG;

    uint8_t value = 0;
    esp_err_t err = pi4ioe5v6408_read_port(handle, &value);
    if (err != ESP_OK) return err;

    *high = (value & (1u << pin)) != 0;
    return ESP_OK;
}

esp_err_t pi4ioe5v6408_read_interrupt_status(pi4ioe5v6408_handle_t handle,
                                             uint8_t *changed)
{
    if (!handle || !changed) return ESP_ERR_INVALID_ARG;
    /* Reading clears the latch, so this both reports and acknowledges. */
    return read8(handle, REG_INT_STATUS, changed);
}
