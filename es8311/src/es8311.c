#include "es8311.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Registers this driver programs. The part has more; es8311_read_reg() and
 * es8311_write_reg() reach the rest. */
#define REG_RESET         0x00
#define REG_CLK_MGR_1     0x01   /* clock gates and source select */
#define REG_CLK_MGR_2     0x02   /* pre-divider and pre-multiplier */
#define REG_CLK_MGR_3     0x03   /* ADC fs mode and oversampling */
#define REG_CLK_MGR_4     0x04   /* DAC oversampling */
#define REG_CLK_MGR_5     0x05   /* ADC and DAC clock dividers */
#define REG_CLK_MGR_6     0x06   /* BCLK divider (master mode) and polarity */
#define REG_CLK_MGR_7     0x07   /* LRCK divider high (master mode) */
#define REG_CLK_MGR_8     0x08   /* LRCK divider low (master mode) */
#define REG_SDP_IN        0x09
#define REG_SDP_OUT       0x0A
#define REG_SYSTEM_0D     0x0D   /* analog power */
#define REG_SYSTEM_0E     0x0E   /* PGA and ADC modulator power */
#define REG_SYSTEM_12     0x12   /* DAC power */
#define REG_SYSTEM_13     0x13   /* output driver enable */
#define REG_SYSTEM_14     0x14   /* mic input select and PGA gain */
#define REG_ADC_16        0x16   /* ADC digital gain scale */
#define REG_ADC_17        0x17   /* ADC volume */
#define REG_ADC_1C        0x1C   /* ADC equalizer bypass, DC cancel */
#define REG_DAC_31        0x31   /* DAC mute */
#define REG_DAC_32        0x32   /* DAC volume */
#define REG_DAC_37        0x37   /* DAC equalizer bypass */
#define REG_CHIP_ID1      0xFD
#define REG_CHIP_ID2      0xFE
#define REG_CHIP_VERSION  0xFF

#define CHIP_ID1 0x83
#define CHIP_ID2 0x11

#define RESET_ALL        0x1F
#define RESET_CSM_ON     0x80    /* chip state machine on: the power-on command */
#define RESET_MSC_MASTER 0x40    /* clear for slave serial port */

#define CLK1_ALL_ON      0x3F
#define CLK1_MCLK_FROM_BCLK 0x80

#define DAC31_MUTE       0x60    /* bits 6 and 5: mute both DAC stages */

#define I2C_TIMEOUT_MS   1000
#define RESET_WAIT_MS    20

struct es8311_dev_t {
    i2c_master_dev_handle_t dev;
};

const char *es8311_stage_name(es8311_stage_t stage)
{
    switch (stage) {
        case ES8311_STAGE_NONE:     return "none";
        case ES8311_STAGE_IDENTIFY: return "identifying the part";
        case ES8311_STAGE_RESET:    return "resetting";
        case ES8311_STAGE_CLOCK:    return "configuring the clock";
        case ES8311_STAGE_FORMAT:   return "configuring the audio format";
        case ES8311_STAGE_ANALOG:   return "powering the analog blocks";
        case ES8311_STAGE_ACCESS:   return "accessing a register";
    }
    return "unknown";
}

esp_err_t es8311_create(i2c_master_dev_handle_t dev, es8311_handle_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    struct es8311_dev_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    d->dev = dev;
    *out = d;
    return ESP_OK;
}

void es8311_delete(es8311_handle_t handle)
{
    free(handle);
}

esp_err_t es8311_set_device(es8311_handle_t handle, i2c_master_dev_handle_t dev)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->dev = dev;
    return ESP_OK;
}

esp_err_t es8311_write_reg(es8311_handle_t handle, uint8_t reg, uint8_t value)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(handle->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t es8311_read_reg(es8311_handle_t handle, uint8_t reg, uint8_t *value)
{
    if (!handle || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(handle->dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t fail(es8311_report_t *r, es8311_stage_t stage, uint8_t reg, esp_err_t err)
{
    if (r) {
        r->failed_stage = stage;
        r->reg = reg;
    }
    return err;
}

/* Write, and on failure say which register of which stage. */
#define W(stage, reg, value)                                                   \
    do {                                                                       \
        esp_err_t e_ = es8311_write_reg(handle, (reg), (value));               \
        if (e_ != ESP_OK) {                                                    \
            return fail(report, (stage), (reg), e_);                           \
        }                                                                      \
    } while (0)

esp_err_t es8311_identify(es8311_handle_t handle, es8311_id_t *id,
                          es8311_report_t *report)
{
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    es8311_id_t local;
    if (!id) {
        id = &local;
    }
    memset(id, 0, sizeof(*id));

    esp_err_t err = es8311_read_reg(handle, REG_CHIP_ID1, &id->chip_id1);
    if (err != ESP_OK) {
        return fail(report, ES8311_STAGE_IDENTIFY, REG_CHIP_ID1, err);
    }
    err = es8311_read_reg(handle, REG_CHIP_ID2, &id->chip_id2);
    if (err != ESP_OK) {
        return fail(report, ES8311_STAGE_IDENTIFY, REG_CHIP_ID2, err);
    }
    err = es8311_read_reg(handle, REG_CHIP_VERSION, &id->version);
    if (err != ESP_OK) {
        return fail(report, ES8311_STAGE_IDENTIFY, REG_CHIP_VERSION, err);
    }

    if (id->chip_id1 != CHIP_ID1 || id->chip_id2 != CHIP_ID2) {
        return fail(report, ES8311_STAGE_IDENTIFY, REG_CHIP_ID1, ESP_ERR_ES8311_WRONG_PART);
    }
    return ESP_OK;
}

esp_err_t es8311_start(es8311_handle_t handle, const es8311_format_t *fmt,
                       es8311_report_t *report)
{
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!handle || !fmt) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Work out the clock first: a format the part cannot reach should fail
     * before the reset throws away whatever state it was in. */
    es8311_clock_plan_t plan;
    esp_err_t err = es8311_clock_plan(fmt->clk_hz, fmt->rate_hz, &plan);
    if (err != ESP_OK) {
        return fail(report, ES8311_STAGE_CLOCK, REG_CLK_MGR_2, err);
    }
    uint8_t word_length;
    if (!es8311_word_length_field(fmt->bits, &word_length)) {
        return fail(report, ES8311_STAGE_FORMAT, REG_SDP_IN, ESP_ERR_INVALID_ARG);
    }

    W(ES8311_STAGE_RESET, REG_RESET, RESET_ALL);
    vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS) + 1);
    W(ES8311_STAGE_RESET, REG_RESET, 0x00);
    W(ES8311_STAGE_RESET, REG_RESET, RESET_CSM_ON);

    /* Clock source and all gates on, then the dividers. With no MCLK pin the
     * part takes SCLK as its master clock and the pre-multiplier does the rest. */
    uint8_t clk1 = CLK1_ALL_ON;
    if (fmt->clk_src == ES8311_CLK_BCLK) {
        clk1 |= CLK1_MCLK_FROM_BCLK;
    }
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_1, clk1);
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_2, es8311_reg02_field(&plan));
    /* Single-speed, 16x oversampling on both converters, no extra divide. */
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_3, 0x10);
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_4, 0x10);
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_5, 0x00);
    /* The BCLK and LRCK dividers only matter in master mode, which this driver
     * never selects; they are written to the values the reference tables use so
     * a register dump matches a known-good one. SCLK is not inverted. */
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_6, 0x03);
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_7, 0x00);
    W(ES8311_STAGE_CLOCK, REG_CLK_MGR_8, 0xFF);

    /* Slave serial port (bit 6 clear), I2S framing (low bits of 0x09/0x0A clear). */
    uint8_t reg00 = RESET_CSM_ON & (uint8_t)~RESET_MSC_MASTER;
    W(ES8311_STAGE_FORMAT, REG_RESET, reg00);
    W(ES8311_STAGE_FORMAT, REG_SDP_IN, word_length);
    W(ES8311_STAGE_FORMAT, REG_SDP_OUT, word_length);

    /* Muted and at the bottom of the scale before anything can reach the
     * speaker: power-up is when a codec pops, and the caller opens the volume. */
    W(ES8311_STAGE_ANALOG, REG_DAC_32, ES8311_DAC_CODE_MIN);
    W(ES8311_STAGE_ANALOG, REG_DAC_31, DAC31_MUTE);

    W(ES8311_STAGE_ANALOG, REG_SYSTEM_0D, 0x01);  /* analog circuitry on */
    W(ES8311_STAGE_ANALOG, REG_SYSTEM_0E, 0x02);  /* PGA and ADC modulator on */
    W(ES8311_STAGE_ANALOG, REG_SYSTEM_12, 0x00);  /* DAC on */
    W(ES8311_STAGE_ANALOG, REG_SYSTEM_13, 0x10);  /* output driver on */
    W(ES8311_STAGE_ANALOG, REG_ADC_1C, 0x6A);     /* ADC EQ bypass, DC cancel */
    W(ES8311_STAGE_ANALOG, REG_DAC_37, 0x08);     /* DAC EQ bypass */

    return ESP_OK;
}

esp_err_t es8311_stop(es8311_handle_t handle, es8311_report_t *report)
{
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Mute first, then drop the analog blocks, so the output does not thump. */
    W(ES8311_STAGE_ANALOG, REG_DAC_31, DAC31_MUTE);
    W(ES8311_STAGE_ANALOG, REG_SYSTEM_12, 0x02);  /* DAC powered down */
    W(ES8311_STAGE_ANALOG, REG_SYSTEM_0E, 0xFF);  /* PGA and modulator powered down */
    W(ES8311_STAGE_ANALOG, REG_SYSTEM_0D, 0xFA);  /* analog circuitry down */
    return ESP_OK;
}

esp_err_t es8311_set_dac_volume(es8311_handle_t handle, uint8_t code)
{
    return es8311_write_reg(handle, REG_DAC_32, code);
}

esp_err_t es8311_get_dac_volume(es8311_handle_t handle, uint8_t *code)
{
    return es8311_read_reg(handle, REG_DAC_32, code);
}

esp_err_t es8311_set_mute(es8311_handle_t handle, bool mute)
{
    uint8_t value;
    esp_err_t err = es8311_read_reg(handle, REG_DAC_31, &value);
    if (err != ESP_OK) {
        return err;
    }
    if (mute) {
        value |= DAC31_MUTE;
    } else {
        value &= (uint8_t)~DAC31_MUTE;
    }
    return es8311_write_reg(handle, REG_DAC_31, value);
}

esp_err_t es8311_set_mic(es8311_handle_t handle, const es8311_mic_t *mic)
{
    if (!handle || !mic || mic->pga_code > 10 || mic->adc_scale > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Bits 5:4 = 01 selects MIC1P/MIC1N; bit 6 selects the PDM microphone. */
    uint8_t reg14 = (uint8_t)(0x10 | mic->pga_code | (mic->digital ? 0x40 : 0x00));

    esp_err_t err = es8311_write_reg(handle, REG_ADC_17, 0xC8);
    if (err != ESP_OK) {
        return err;
    }
    err = es8311_write_reg(handle, REG_ADC_16, mic->adc_scale);
    if (err != ESP_OK) {
        return err;
    }
    return es8311_write_reg(handle, REG_SYSTEM_14, reg14);
}

esp_err_t es8311_dump(es8311_handle_t handle, uint8_t out[ES8311_REG_COUNT],
                      es8311_report_t *report)
{
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int reg = 0; reg < ES8311_REG_COUNT; reg++) {
        esp_err_t err = es8311_read_reg(handle, (uint8_t)reg, &out[reg]);
        if (err != ESP_OK) {
            return fail(report, ES8311_STAGE_ACCESS, (uint8_t)reg, err);
        }
    }
    return ESP_OK;
}
