#ifndef ES8311_H
#define ES8311_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "es8311_calc.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everest ES8311 mono audio codec: a DAC for a speaker amplifier and an ADC for
 * an analog or PDM microphone, controlled over I2C, with audio over I2S.
 *
 * Returns facts rather than formatted text. Every call that collapses a
 * sequence says which step failed in an es8311_report_t, so the caller can name
 * the right thing to fix. This driver runs the part as an I2S *slave* only: the
 * host owns BCLK and LRCK.
 *
 * It does not own the bus. `dev` is an I2C device handle the caller created at
 * ES8311_I2C_ADDR_DEFAULT (or the alternate) and keeps alive; it may be NULL at
 * creation, and every call that touches the bus returns ESP_ERR_INVALID_STATE
 * until es8311_set_device() supplies one.
 */

#define ES8311_I2C_ADDR_DEFAULT 0x18   /* CE pin low; 0x19 when high */
#define ES8311_I2C_ADDR_ALT     0x19

#define ES8311_REG_COUNT 0x46          /* registers 0x00..0x45 */

typedef struct es8311_dev_t *es8311_handle_t;

typedef enum {
    ES8311_STAGE_NONE = 0,
    ES8311_STAGE_IDENTIFY,   /* chip ID registers */
    ES8311_STAGE_RESET,
    ES8311_STAGE_CLOCK,      /* clock source, dividers, converter ratios */
    ES8311_STAGE_FORMAT,     /* slave mode, word length */
    ES8311_STAGE_ANALOG,     /* powering the analog blocks and the DAC */
    ES8311_STAGE_ACCESS,     /* a single register read/write */
} es8311_stage_t;

typedef struct {
    es8311_stage_t failed_stage;
    uint8_t reg;             /* the register being accessed when it failed */
} es8311_report_t;

typedef enum {
    ES8311_CLK_MCLK_PIN,     /* an MCLK pin is wired and running */
    ES8311_CLK_BCLK,         /* no MCLK: the part derives its clock from SCLK */
} es8311_clk_src_t;

typedef struct {
    uint32_t rate_hz;        /* ES8311_RATE_MIN_HZ..ES8311_RATE_MAX_HZ */
    uint8_t bits;            /* word length on the wire: 16, 18, 20, 24 or 32 */
    es8311_clk_src_t clk_src;
    /* Frequency of the pin the clock comes from: MCLK for ES8311_CLK_MCLK_PIN,
     * the bit clock for ES8311_CLK_BCLK. The host's I2S driver knows it. */
    uint32_t clk_hz;
} es8311_format_t;

typedef struct {
    uint8_t chip_id1;        /* register 0xFD, expected 0x83 */
    uint8_t chip_id2;        /* register 0xFE, expected 0x11 */
    uint8_t version;         /* register 0xFF */
} es8311_id_t;

esp_err_t es8311_create(i2c_master_dev_handle_t dev, es8311_handle_t *out);
void      es8311_delete(es8311_handle_t handle);
esp_err_t es8311_set_device(es8311_handle_t handle, i2c_master_dev_handle_t dev);

/* Read the chip ID. Fills `id` even when it returns ES8311_WRONG_PART, so the
 * caller can say what answered instead. */
esp_err_t es8311_identify(es8311_handle_t handle, es8311_id_t *id,
                          es8311_report_t *report);

/*
 * Reset, clock, format, power up -- everything short of volume. Leaves the DAC
 * at the lowest volume code and muted, so a speaker does not hear whatever the
 * previous owner of the part left behind; es8311_set_dac_volume() and
 * es8311_set_mute() open it.
 *
 * Fails with ES8311_UNSUPPORTED_CLOCK at ES8311_STAGE_CLOCK when no divider
 * reaches 256 x the sample rate from fmt->clk_hz.
 */
esp_err_t es8311_start(es8311_handle_t handle, const es8311_format_t *fmt,
                       es8311_report_t *report);

/* Mute the converters and power the analog blocks down. */
esp_err_t es8311_stop(es8311_handle_t handle, es8311_report_t *report);

/* Raw DAC volume code; see ES8311_DAC_CODE_0DB in es8311_calc.h. */
esp_err_t es8311_set_dac_volume(es8311_handle_t handle, uint8_t code);
esp_err_t es8311_get_dac_volume(es8311_handle_t handle, uint8_t *code);
esp_err_t es8311_set_mute(es8311_handle_t handle, bool mute);

typedef struct {
    bool digital;            /* PDM microphone on the DMIC pins, not the analog input */
    uint8_t pga_code;        /* 0..10: analog preamp, 3 dB per step */
    uint8_t adc_scale;       /* 0..7: digital gain after the ADC, 6 dB per step */
} es8311_mic_t;

esp_err_t es8311_set_mic(es8311_handle_t handle, const es8311_mic_t *mic);

esp_err_t es8311_read_reg(es8311_handle_t handle, uint8_t reg, uint8_t *value);
esp_err_t es8311_write_reg(es8311_handle_t handle, uint8_t reg, uint8_t value);

/* Read registers 0x00..ES8311_REG_COUNT-1. On failure `report->reg` names the one. */
esp_err_t es8311_dump(es8311_handle_t handle, uint8_t out[ES8311_REG_COUNT],
                      es8311_report_t *report);

const char *es8311_stage_name(es8311_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* ES8311_H */
