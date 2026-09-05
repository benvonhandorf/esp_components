#include "aw9523b.h"

#include <stdlib.h>
#include <string.h>

#define REG_P0_INPUT    0x00
#define REG_P1_INPUT    0x01
#define REG_P0_OUTPUT   0x02
#define REG_P1_OUTPUT   0x03
#define REG_P0_CONFIG   0x04   /* 1 = input */
#define REG_P1_CONFIG   0x05
#define REG_P0_INT_MASK 0x06   /* 1 = masked */
#define REG_P1_INT_MASK 0x07
#define REG_ID          0x10
#define REG_GCR         0x11
#define REG_LED_MODE_P0 0x12   /* 1 = GPIO, 0 = LED */
#define REG_LED_MODE_P1 0x13

#define GCR_P0_PUSH_PULL (1 << 4)
#define CHIP_ID          0x23

#define I2C_TIMEOUT_MS 1000

struct aw9523b_dev_t {
    i2c_master_dev_handle_t dev;
    aw9523b_config_t cfg;
    /* Shadow of the output registers: the part's output register cannot be read
     * back (reading a port returns the *input* register), so setting one pin
     * without disturbing its neighbours needs the last written value kept here. */
    uint8_t out[2];
};

const char *aw9523b_stage_name(aw9523b_stage_t stage)
{
    switch (stage) {
        case AW9523B_STAGE_NONE:      return "none";
        case AW9523B_STAGE_IDENTIFY:  return "identifying the part";
        case AW9523B_STAGE_PRESET:    return "establishing output levels";
        case AW9523B_STAGE_CONFIGURE: return "configuring";
    }
    return "unknown";
}

static esp_err_t write8(struct aw9523b_dev_t *d, uint8_t reg, uint8_t value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t read8(struct aw9523b_dev_t *d, uint8_t reg, uint8_t *value)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(d->dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t fail(aw9523b_report_t *report, aw9523b_stage_t stage, esp_err_t err)
{
    if (report) report->failed_stage = stage;
    return err;
}

static esp_err_t configure(struct aw9523b_dev_t *d, aw9523b_report_t *report)
{
    if (report) report->failed_stage = AW9523B_STAGE_NONE;
    if (!d->dev) return ESP_ERR_INVALID_STATE;

    uint8_t id = 0;
    esp_err_t err = read8(d, REG_ID, &id);
    if (err != ESP_OK) return fail(report, AW9523B_STAGE_IDENTIFY, err);
    if (report) report->chip_id = id;
    if (id != CHIP_ID) return fail(report, AW9523B_STAGE_IDENTIFY, ESP_ERR_AW9523B_WRONG_PART);

    /*
     * Output levels first, direction second. Switching direction first lets
     * whatever the output register happens to hold reach the pins -- on a board
     * driving relays that is an audible clack at every reset, and on one driving
     * FETs it can be worse.
     */
    d->out[0] = d->cfg.port0_initial;
    d->out[1] = d->cfg.port1_initial;
    err = write8(d, REG_P0_OUTPUT, d->out[0]);
    if (err == ESP_OK) err = write8(d, REG_P1_OUTPUT, d->out[1]);
    if (err != ESP_OK) return fail(report, AW9523B_STAGE_PRESET, err);

    /* GPIO mode on every pin: the alternative is the constant-current LED sink,
     * which this driver does not cover. */
    err = write8(d, REG_LED_MODE_P0, 0xFF);
    if (err == ESP_OK) err = write8(d, REG_LED_MODE_P1, 0xFF);
    if (err == ESP_OK) {
        err = write8(d, REG_GCR, d->cfg.port0_push_pull ? GCR_P0_PUSH_PULL : 0x00);
    }
    if (err == ESP_OK) err = write8(d, REG_P0_CONFIG, d->cfg.port0_inputs);
    if (err == ESP_OK) err = write8(d, REG_P1_CONFIG, d->cfg.port1_inputs);
    if (err != ESP_OK) return fail(report, AW9523B_STAGE_CONFIGURE, err);

    return ESP_OK;
}

esp_err_t aw9523b_create(const aw9523b_config_t *cfg, aw9523b_handle_t *out,
                         aw9523b_report_t *report)
{
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (report) memset(report, 0, sizeof(*report));

    struct aw9523b_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->cfg = *cfg;
    d->dev = cfg->dev;
    d->out[0] = cfg->port0_initial;
    d->out[1] = cfg->port1_initial;

    if (d->dev) {
        esp_err_t err = configure(d, report);
        if (err != ESP_OK) { free(d); return err; }
    }

    *out = d;
    return ESP_OK;
}

void aw9523b_delete(aw9523b_handle_t handle) { free(handle); }

esp_err_t aw9523b_set_device(aw9523b_handle_t handle, i2c_master_dev_handle_t dev,
                             aw9523b_report_t *report)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (report) memset(report, 0, sizeof(*report));
    handle->dev = dev;
    return dev ? configure(handle, report) : ESP_OK;
}

esp_err_t aw9523b_write_port(aw9523b_handle_t handle, uint8_t port, uint8_t value)
{
    if (!handle || port > AW9523B_PORT1) return ESP_ERR_INVALID_ARG;
    esp_err_t err = write8(handle, port == AW9523B_PORT0 ? REG_P0_OUTPUT : REG_P1_OUTPUT,
                           value);
    if (err == ESP_OK) {
        handle->out[port] = value;
    }
    return err;
}

esp_err_t aw9523b_read_port(aw9523b_handle_t handle, uint8_t port, uint8_t *value)
{
    if (!handle || !value || port > AW9523B_PORT1) return ESP_ERR_INVALID_ARG;
    return read8(handle, port == AW9523B_PORT0 ? REG_P0_INPUT : REG_P1_INPUT, value);
}

esp_err_t aw9523b_set_pin(aw9523b_handle_t handle, uint8_t port, uint8_t pin, bool high)
{
    if (!handle || port > AW9523B_PORT1 || pin > 7) return ESP_ERR_INVALID_ARG;

    /* From the shadow, not from the part: reading a port returns the input
     * register, so a read-modify-write would take the pin states of the inputs
     * and write them back over the outputs. */
    uint8_t value = handle->out[port];
    if (high) {
        value |= (uint8_t)(1u << pin);
    } else {
        value &= (uint8_t)~(1u << pin);
    }
    return aw9523b_write_port(handle, port, value);
}

esp_err_t aw9523b_get_pin(aw9523b_handle_t handle, uint8_t port, uint8_t pin, bool *high)
{
    if (!handle || !high || pin > 7) return ESP_ERR_INVALID_ARG;

    uint8_t value = 0;
    esp_err_t err = aw9523b_read_port(handle, port, &value);
    if (err != ESP_OK) return err;

    *high = (value & (1u << pin)) != 0;
    return ESP_OK;
}

esp_err_t aw9523b_clear_interrupt(aw9523b_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    /* Reading both input ports clears the latched change. */
    uint8_t discard;
    esp_err_t err = read8(handle, REG_P0_INPUT, &discard);
    if (err == ESP_OK) err = read8(handle, REG_P1_INPUT, &discard);
    return err;
}
