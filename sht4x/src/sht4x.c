/*
 * Sensirion SHT4x humidity and temperature sensor.
 *
 * See include/sht4x.h for the API and README.md for the protocol quirks that
 * are not obvious from the datasheet.
 *
 * The bus protocol differs from a conventional register device: there is no
 * register pointer. A command byte is written as its own transaction, the
 * sensor is then given time to measure, and the result is read back in a
 * *separate* transaction. Issuing the read too early -- as a repeated-start
 * write-then-read does -- makes the sensor NACK the read header (datasheet
 * Table 8), so i2c_master_transmit_receive() cannot be used here.
 */
#include "sht4x_priv.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

const sht4x_repeatability_t sht4x_repeatabilities[3] = {
    SHT4X_REPEATABILITY_HIGH,
    SHT4X_REPEATABILITY_MEDIUM,
    SHT4X_REPEATABILITY_LOW,
};

const sht4x_heater_t sht4x_heaters[6] = {
    SHT4X_HEATER_200MW_1S,  SHT4X_HEATER_200MW_100MS,
    SHT4X_HEATER_110MW_1S,  SHT4X_HEATER_110MW_100MS,
    SHT4X_HEATER_20MW_1S,   SHT4X_HEATER_20MW_100MS,
};

const char *sht4x_repeatability_name(sht4x_repeatability_t r)
{
    switch (r) {
    case SHT4X_REPEATABILITY_HIGH:   return "high";
    case SHT4X_REPEATABILITY_MEDIUM: return "medium";
    case SHT4X_REPEATABILITY_LOW:    return "low";
    }
    return NULL;
}

esp_err_t sht4x_repeatability_from_name(const char *name,
                                        sht4x_repeatability_t *out)
{
    if (!name || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * "med" is accepted alongside "medium" because the console has always
     * taken it; an abbreviation the caller's users already type is part of the
     * encoding, not of the formatting.
     */
    if (strcasecmp(name, "high") == 0) {
        *out = SHT4X_REPEATABILITY_HIGH;
    } else if (strcasecmp(name, "medium") == 0 || strcasecmp(name, "med") == 0) {
        *out = SHT4X_REPEATABILITY_MEDIUM;
    } else if (strcasecmp(name, "low") == 0) {
        *out = SHT4X_REPEATABILITY_LOW;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

int sht4x_heater_power_mw(sht4x_heater_t h)
{
    switch (h) {
    case SHT4X_HEATER_200MW_1S:
    case SHT4X_HEATER_200MW_100MS: return 200;
    case SHT4X_HEATER_110MW_1S:
    case SHT4X_HEATER_110MW_100MS: return 110;
    case SHT4X_HEATER_20MW_1S:
    case SHT4X_HEATER_20MW_100MS:  return 20;
    }
    return -1;
}

int sht4x_heater_duration_ms(sht4x_heater_t h)
{
    switch (h) {
    case SHT4X_HEATER_200MW_1S:
    case SHT4X_HEATER_110MW_1S:
    case SHT4X_HEATER_20MW_1S:     return 1000;
    case SHT4X_HEATER_200MW_100MS:
    case SHT4X_HEATER_110MW_100MS:
    case SHT4X_HEATER_20MW_100MS:  return 100;
    }
    return -1;
}

esp_err_t sht4x_heater_from_values(int power_mw, int duration_ms,
                                   sht4x_heater_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(sht4x_heaters) / sizeof(sht4x_heaters[0]); i++) {
        if (sht4x_heater_power_mw(sht4x_heaters[i]) == power_mw &&
            sht4x_heater_duration_ms(sht4x_heaters[i]) == duration_ms) {
            *out = sht4x_heaters[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

/* Table 7: CRC-8, polynomial 0x31, initialised to 0xFF, no reflection,
 * no final XOR. The datasheet's worked example is CRC(0xBEEF) = 0x92. */
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }

    return crc;
}

static void wait_ms(uint32_t ms)
{
    /*
     * vTaskDelay(n) guarantees only (n-1) full tick periods: the first tick
     * boundary can arrive immediately, so vTaskDelay(1) may return almost at
     * once. With a 10 ms tick that let the read overtake an 8.3 ms
     * measurement, and the sensor NACKed the read header. The extra tick makes
     * the delay at least the requested time.
     */
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

/*
 * Write a command, wait, and optionally read the reply.
 *
 * Pass wait_time_ms = 0 and length = 0 for commands with no response, such as
 * the soft reset.
 */
static esp_err_t send_command(sht4x_handle_t handle, uint8_t command,
                              uint32_t wait_time_ms, uint8_t *buffer, size_t length)
{
    esp_err_t err = i2c_master_transmit(handle->dev, &command, 1, XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    wait_ms(wait_time_ms);

    if (length == 0) {
        return ESP_OK;
    }

    return i2c_master_receive(handle->dev, buffer, length, XFER_TIMEOUT_MS);
}

/* Validate both CRCs and return the two data words. */
static esp_err_t decode_response(const uint8_t *raw, uint16_t *first,
                                 uint16_t *second, sht4x_crc_error_t *crc_error)
{
    const uint8_t first_computed = crc8(&raw[0], 2);
    if (first_computed != raw[2]) {
        if (crc_error) {
            crc_error->word = 1;
            crc_error->received = raw[2];
            crc_error->computed = first_computed;
        }
        return ESP_ERR_SHT4X_CRC;
    }

    const uint8_t second_computed = crc8(&raw[3], 2);
    if (second_computed != raw[5]) {
        if (crc_error) {
            crc_error->word = 2;
            crc_error->received = raw[5];
            crc_error->computed = second_computed;
        }
        return ESP_ERR_SHT4X_CRC;
    }

    *first = (uint16_t)((raw[0] << 8) | raw[1]);
    *second = (uint16_t)((raw[3] << 8) | raw[4]);
    return ESP_OK;
}

/* Datasheet section 4.6, equations 1 and 2. */
static double ticks_to_celsius(uint16_t ticks)
{
    return -45.0 + 175.0 * ticks / 65535.0;
}

static double ticks_to_humidity(uint16_t ticks)
{
    return -6.0 + 125.0 * ticks / 65535.0;
}

esp_err_t sht4x_create(const sht4x_config_t *config, sht4x_handle_t *out)
{
    if (!config || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    struct sht4x_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    dev->dev = config->dev;
    *out = dev;
    return ESP_OK;
}

esp_err_t sht4x_delete(sht4x_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The I2C device handle belongs to the caller; it is not deleted here. */
    free(handle);
    return ESP_OK;
}

esp_err_t sht4x_set_device(sht4x_handle_t handle, i2c_master_dev_handle_t dev)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->dev = dev;
    return ESP_OK;
}

/* Shared by measure and heater, both of which return a measurement. */
static esp_err_t measure_with(sht4x_handle_t handle, uint8_t command,
                              uint32_t wait_time_ms, sht4x_measurement_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    *out = (sht4x_measurement_t){0};

    uint8_t raw[RESPONSE_LEN];
    esp_err_t err = send_command(handle, command, wait_time_ms, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    uint16_t temperature_ticks = 0;
    uint16_t humidity_ticks = 0;
    err = decode_response(raw, &temperature_ticks, &humidity_ticks,
                          &out->crc_error);
    if (err != ESP_OK) {
        return err;
    }

    out->temperature_ticks = temperature_ticks;
    out->humidity_ticks = humidity_ticks;
    out->temperature_c = ticks_to_celsius(temperature_ticks);
    out->humidity_pct = ticks_to_humidity(humidity_ticks);

    out->humidity_pct_cropped = out->humidity_pct;
    if (out->humidity_pct_cropped > 100.0) {
        out->humidity_pct_cropped = 100.0;
    } else if (out->humidity_pct_cropped < 0.0) {
        out->humidity_pct_cropped = 0.0;
    }
    out->humidity_was_cropped = (out->humidity_pct_cropped != out->humidity_pct);

    return ESP_OK;
}

esp_err_t sht4x_measure(sht4x_handle_t handle, sht4x_repeatability_t r,
                        sht4x_measurement_t *out)
{
    if (sht4x_repeatability_name(r) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return measure_with(handle, (uint8_t)r, MEASURE_WAIT_MS, out);
}

esp_err_t sht4x_run_heater(sht4x_handle_t handle, sht4x_heater_t h,
                           sht4x_measurement_t *out)
{
    const int duration_ms = sht4x_heater_duration_ms(h);
    if (duration_ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t wait_time =
        (duration_ms == 1000) ? HEATER_LONG_WAIT_MS : HEATER_SHORT_WAIT_MS;
    return measure_with(handle, (uint8_t)h, wait_time, out);
}

esp_err_t sht4x_read_serial(sht4x_handle_t handle, uint32_t *out,
                            sht4x_crc_error_t *crc_error)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[RESPONSE_LEN];
    esp_err_t err = send_command(handle, CMD_READ_SERIAL, SERIAL_WAIT_MS,
                                 raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    uint16_t high = 0;
    uint16_t low = 0;
    err = decode_response(raw, &high, &low, crc_error);
    if (err != ESP_OK) {
        return err;
    }

    *out = ((uint32_t)high << 16) | low;
    return ESP_OK;
}

esp_err_t sht4x_soft_reset(sht4x_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_command(handle, CMD_SOFT_RESET, RESET_WAIT_MS, NULL, 0);
}
