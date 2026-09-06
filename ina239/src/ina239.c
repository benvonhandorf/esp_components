/*
 * TI INA239 current / voltage / power monitor, SPI.
 *
 * See include/ina239.h for the API and README.md for the datasheet quirks that
 * are not obvious from SLYS027A.
 */
#include "ina239_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_rom_sys.h"

/* The longest frame is a command byte plus the 24-bit POWER register. */
#define MAX_FRAME 4

/*
 * One SPI frame.
 *
 * `spi_device_polling_transmit()` rather than the queued form: every transfer
 * here is at most four bytes, so the interrupt round trip costs more than
 * spinning does, and polling needs no task context.
 */
static esp_err_t xfer(spi_device_handle_t dev, const uint8_t *tx, uint8_t *rx,
                      size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(dev, &t);
}

/*
 * Read a register.
 *
 * A frame is the command byte and then the register, most significant byte
 * first; the device holds MISO low for the command byte (section 7.5.1), so the
 * data begins at rx[1]. There is no burst read and nothing auto-increments --
 * the address travels in every frame -- so each value is its own transaction.
 */
static esp_err_t read_reg(spi_device_handle_t dev, uint8_t reg, uint8_t *out,
                          size_t len)
{
    uint8_t tx[MAX_FRAME] = {CMD_READ(reg)};
    uint8_t rx[MAX_FRAME] = {0};

    esp_err_t err = xfer(dev, tx, rx, len + 1);
    if (err == ESP_OK) {
        memcpy(out, rx + 1, len);
    }
    return err;
}

static esp_err_t read_reg16(spi_device_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t raw[2];
    esp_err_t err = read_reg(dev, reg, raw, sizeof(raw));
    if (err == ESP_OK) {
        *out = (uint16_t)((raw[0] << 8) | raw[1]);
    }
    return err;
}

static esp_err_t read_reg24(spi_device_handle_t dev, uint8_t reg, uint32_t *out)
{
    uint8_t raw[3];
    esp_err_t err = read_reg(dev, reg, raw, sizeof(raw));
    if (err == ESP_OK) {
        *out = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
    }
    return err;
}

/* Every writable register is 16 bits wide, so a write frame is a fixed three
 * bytes (section 7.5.1.1). The old contents come back on MISO; nothing here
 * wants them. */
static esp_err_t write_reg16(spi_device_handle_t dev, uint8_t reg, uint16_t value)
{
    const uint8_t tx[3] = {CMD_WRITE(reg), (uint8_t)(value >> 8),
                           (uint8_t)(value & 0xFF)};
    return xfer(dev, tx, NULL, sizeof(tx));
}

/* ---------------------------------------------------------------- encodings */

const ina239_averaging_t ina239_averagings[8] = {
    INA239_AVG_1,   INA239_AVG_4,   INA239_AVG_16,  INA239_AVG_64,
    INA239_AVG_128, INA239_AVG_256, INA239_AVG_512, INA239_AVG_1024,
};

int ina239_averaging_count(ina239_averaging_t avg)
{
    switch (avg) {
    case INA239_AVG_1:    return 1;
    case INA239_AVG_4:    return 4;
    case INA239_AVG_16:   return 16;
    case INA239_AVG_64:   return 64;
    case INA239_AVG_128:  return 128;
    case INA239_AVG_256:  return 256;
    case INA239_AVG_512:  return 512;
    case INA239_AVG_1024: return 1024;
    }
    return -1;
}

esp_err_t ina239_averaging_from_count(int count, ina239_averaging_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(ina239_averagings) / sizeof(ina239_averagings[0]); i++) {
        if (ina239_averaging_count(ina239_averagings[i]) == count) {
            *out = ina239_averagings[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

double ina239_shunt_lsb_v(ina239_range_t range)
{
    switch (range) {
    case INA239_RANGE_163MV: return VSHUNT_LSB_V_LOW;
    case INA239_RANGE_41MV:  return VSHUNT_LSB_V_HIGH;
    }
    return 0.0;
}

double ina239_range_full_scale_v(ina239_range_t range)
{
    switch (range) {
    case INA239_RANGE_163MV: return INA239_SHUNT_FULL_SCALE_V_LOW;
    case INA239_RANGE_41MV:  return INA239_SHUNT_FULL_SCALE_V_HIGH;
    }
    return 0.0;
}

double ina239_current_lsb_for(double shunt_ohms, ina239_range_t range)
{
    double lsb = ina239_shunt_lsb_v(range);
    if (!(shunt_ohms > 0.0) || lsb == 0.0) {
        return 0.0;
    }
    /*
     * CURRENT_LSB chosen so full scale current exactly fills the CURRENT
     * register: the shunt LSB in volts, divided by the resistance. See the
     * INA239_SHUNT_CAL_VALUE comment for why this makes SHUNT_CAL a constant.
     */
    return lsb / shunt_ohms;
}

spi_device_interface_config_t ina239_device_config(int cs_gpio, int clock_hz)
{
    if (clock_hz <= 0) {
        clock_hz = 1000000;
    }
    if (clock_hz > INA239_SPI_MAX_HZ) {
        clock_hz = INA239_SPI_MAX_HZ;
    }

    spi_device_interface_config_t cfg = {0};
    cfg.mode = INA239_SPI_MODE;
    cfg.clock_speed_hz = clock_hz;
    cfg.spics_io_num = cs_gpio;
    /*
     * Two, not one: a polling transfer that finds the queue empty still needs
     * a slot, and a caller that pipelines two reads should not have to know
     * that. The cost is a pair of descriptors.
     */
    cfg.queue_size = 2;
    return cfg;
}

/* ------------------------------------------------------------------- handle */

esp_err_t ina239_create(const ina239_config_t *config, ina239_handle_t *out)
{
    if (!config || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    struct ina239_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    dev->dev = config->dev;
    /*
     * The shunt, range and averaging supplied here are remembered but not
     * applied: nothing is written until ina239_configure() has proved the part
     * is really an INA239, and reporting a CURRENT_LSB for a part that never
     * answered would be a number with nothing behind it.
     */
    dev->shunt_ohms = config->shunt_ohms;
    dev->range = config->range;
    dev->averaging = config->averaging;

    *out = dev;
    return ESP_OK;
}

esp_err_t ina239_delete(ina239_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The SPI device belongs to the caller; it is not removed here. */
    free(handle);
    return ESP_OK;
}

esp_err_t ina239_set_device(ina239_handle_t handle, spi_device_handle_t dev)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->dev = dev;
    return ESP_OK;
}

bool ina239_is_configured(ina239_handle_t handle)
{
    return handle && handle->configured;
}

double ina239_shunt_ohms(ina239_handle_t handle)
{
    return handle ? handle->shunt_ohms : 0.0;
}

double ina239_current_lsb(ina239_handle_t handle)
{
    return handle ? handle->current_lsb : 0.0;
}

ina239_range_t ina239_range(ina239_handle_t handle)
{
    return handle ? handle->range : INA239_RANGE_163MV;
}

double ina239_full_scale_amps(ina239_handle_t handle)
{
    if (!handle || !(handle->shunt_ohms > 0.0)) {
        return 0.0;
    }
    return ina239_range_full_scale_v(handle->range) / handle->shunt_ohms;
}

/* ---------------------------------------------------------------- configure */

esp_err_t ina239_configure(ina239_handle_t handle, const ina239_config_t *config,
                           ina239_config_report_t *report)
{
    if (report) {
        *report = (ina239_config_report_t){0};
    }
    if (!handle || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(config->shunt_ohms > 0.0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ina239_shunt_lsb_v(config->range) == 0.0 ||
        ina239_averaging_count(config->averaging) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Identity first, and in two steps.
     *
     * MANUFACTURER_ID separates "nothing is on this chip select" from "something
     * is, and it is not from TI" -- an SPI bus with no pull-up and no device
     * reads back a plausible 0x0000 or 0xFFFF rather than failing, so a probe
     * that only checked the transfer would report success against thin air.
     * DEVICE_ID then separates this part from its siblings, which is the more
     * likely mistake: an INA228 or INA238 answers "TI" just as happily and has
     * a different shunt LSB, so it would read low by a factor of four without
     * anything looking wrong.
     */
    uint16_t manufacturer = 0;
    esp_err_t err = read_reg16(handle->dev, REG_MANUFACTURER_ID, &manufacturer);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = INA239_STAGE_PROBE;
        }
        return err;
    }
    if (report) {
        report->manufacturer_id = manufacturer;
    }
    if (manufacturer != INA239_MANUFACTURER_ID_TI) {
        if (report) {
            report->failed_stage = INA239_STAGE_IDENTIFY;
        }
        return ESP_ERR_INA239_WRONG_PART;
    }

    uint16_t device_id = 0;
    err = read_reg16(handle->dev, REG_DEVICE_ID, &device_id);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = INA239_STAGE_PROBE;
        }
        return err;
    }
    if (report) {
        report->device_id = device_id;
        report->die_id = (uint16_t)(device_id >> 4);
        report->revision = (uint8_t)(device_id & 0xF);
    }
    if ((device_id >> 4) != INA239_DEVICE_ID) {
        if (report) {
            report->failed_stage = INA239_STAGE_DEVICE_ID;
        }
        return ESP_ERR_INA239_WRONG_DEVICE;
    }

    /*
     * CONFIG carries the ADC range and nothing else this driver has an opinion
     * about: CONVDLY stays 0, because a start-up delay only helps a board whose
     * rail is still rising, and RST stays 0 so configuring does not silently
     * discard alert thresholds a caller wrote by hand.
     */
    const uint16_t cfg_reg =
        (config->range == INA239_RANGE_41MV) ? CONFIG_ADCRANGE : 0;
    err = write_reg16(handle->dev, REG_CONFIG, cfg_reg);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = INA239_STAGE_CONFIG;
        }
        return err;
    }

    const uint16_t adc_cfg =
        (uint16_t)((ADC_CONFIG_MODE_CONTINUOUS_ALL << ADC_CONFIG_MODE_SHIFT) |
                   (ADC_CONFIG_CT_DEFAULT << ADC_CONFIG_VBUSCT_SHIFT) |
                   (ADC_CONFIG_CT_DEFAULT << ADC_CONFIG_VSHCT_SHIFT) |
                   (ADC_CONFIG_CT_DEFAULT << ADC_CONFIG_VTCT_SHIFT) |
                   (uint16_t)config->averaging);
    err = write_reg16(handle->dev, REG_ADC_CONFIG, adc_cfg);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = INA239_STAGE_ADC_CONFIG;
        }
        return err;
    }

    /*
     * SHUNT_CAL last, and after CONFIG: the ADCRANGE = 1 rule multiplies the
     * calibration by four, so a value written before the range was settled
     * would be right for the wrong range. It happens to be the same constant
     * either way -- see INA239_SHUNT_CAL_VALUE -- but relying on that ordering
     * accident would make a future range-dependent value silently wrong.
     */
    err = write_reg16(handle->dev, REG_SHUNT_CAL, INA239_SHUNT_CAL_VALUE);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = INA239_STAGE_SHUNT_CAL;
        }
        return err;
    }

    handle->shunt_ohms = config->shunt_ohms;
    handle->range = config->range;
    handle->averaging = config->averaging;
    handle->config_written = cfg_reg;
    handle->adc_config_written = adc_cfg;
    handle->current_lsb = ina239_current_lsb_for(config->shunt_ohms, config->range);
    handle->configured = true;

    if (report) {
        report->current_lsb = handle->current_lsb;
        report->full_scale_amps = ina239_full_scale_amps(handle);
    }

    return ESP_OK;
}

esp_err_t ina239_reset(ina239_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = write_reg16(handle->dev, REG_CONFIG, CONFIG_RST);
    if (err != ESP_OK) {
        return err;
    }

    /* RST self-clears, but everything it cleared is gone: the handle no longer
     * describes the device, so say so rather than letting the next read scale
     * against a calibration the part has forgotten. */
    handle->configured = false;
    esp_rom_delay_us(RESET_SETTLE_MS * 1000);
    return ESP_OK;
}

/* --------------------------------------------------------------------- read */

esp_err_t ina239_read(ina239_handle_t handle, ina239_reading_t *out)
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
    uint16_t shunt_cal = 0, diag = 0, config = 0, adc_config = 0;
    uint32_t power_raw = 0;
    esp_err_t err;

    /*
     * The three configuration registers are read back with the measurements
     * rather than trusted. A part that browned out and reset is otherwise
     * invisible: it goes on answering, on the wide range, and every current it
     * reports is a quarter of the truth on a board configured for 41 mV.
     */
    if ((err = read_reg16(handle->dev, REG_VBUS, &vbus_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_VSHUNT, &vshunt_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_DIETEMP, &dietemp_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_CURRENT, &current_raw)) != ESP_OK ||
        (err = read_reg24(handle->dev, REG_POWER, &power_raw)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_CONFIG, &config)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_ADC_CONFIG, &adc_config)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_SHUNT_CAL, &shunt_cal)) != ESP_OK ||
        (err = read_reg16(handle->dev, REG_DIAG_ALRT, &diag)) != ESP_OK) {
        return err;
    }

    /*
     * VBUS is two's complement but never negative; VSHUNT and CURRENT are
     * signed; DIETEMP is a signed 12-bit value living in bits 15:4, so it is
     * sign-extended as 16 bits first and then shifted down -- shifting an
     * unsigned value first would drop the sign and read -1 degC as +255.9.
     */
    out->bus_v = (int16_t)vbus_raw * VBUS_LSB_V;
    out->shunt_v = (int16_t)vshunt_raw * ina239_shunt_lsb_v(handle->range);
    out->temp_c = ((int16_t)dietemp_raw >> 4) * DIETEMP_LSB_C;
    out->current_a = (int16_t)current_raw * handle->current_lsb;
    out->power_w = power_raw * POWER_COEFFICIENT * handle->current_lsb;

    out->config = config;
    out->adc_config = adc_config;
    out->shunt_cal = shunt_cal;
    out->diag = diag;

    out->trim_checksum_ok = (diag & DIAG_MEMSTAT) != 0;
    out->math_overflow = (diag & DIAG_MATHOF) != 0;
    out->conversion_ready = (diag & DIAG_CNVRF) != 0;
    /* SHUNT_CAL[15] is reserved and reads 0, so a bare compare is safe. CONFIG
     * bits 14 and 3:0 are reserved and read 0, and everything this driver
     * leaves alone in them is 0 as written, so the same holds there. */
    out->shunt_cal_matches = (shunt_cal == INA239_SHUNT_CAL_VALUE);
    out->config_intact = out->shunt_cal_matches &&
                         config == handle->config_written &&
                         adc_config == handle->adc_config_written;

    return ESP_OK;
}

/* ----------------------------------------------------------- raw access */

esp_err_t ina239_read_register(ina239_handle_t handle, uint8_t reg, uint16_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return read_reg16(handle->dev, reg, out);
}

esp_err_t ina239_write_register(ina239_handle_t handle, uint8_t reg, uint16_t value)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_reg16(handle->dev, reg, value);
}

static const ina239_register_info_t s_registers[] = {
    {REG_CONFIG, "CONFIG", 16},
    {REG_ADC_CONFIG, "ADC_CONFIG", 16},
    {REG_SHUNT_CAL, "SHUNT_CAL", 16},
    {REG_VSHUNT, "VSHUNT", 16},
    {REG_VBUS, "VBUS", 16},
    {REG_DIETEMP, "DIETEMP", 16},
    {REG_CURRENT, "CURRENT", 16},
    {REG_POWER, "POWER", 24},
    {REG_DIAG_ALRT, "DIAG_ALRT", 16},
    {REG_SOVL, "SOVL", 16},
    {REG_SUVL, "SUVL", 16},
    {REG_BOVL, "BOVL", 16},
    {REG_BUVL, "BUVL", 16},
    {REG_TEMP_LIMIT, "TEMP_LIMIT", 16},
    {REG_PWR_LIMIT, "PWR_LIMIT", 16},
    {REG_MANUFACTURER_ID, "MANUFACTURER_ID", 16},
    {REG_DEVICE_ID, "DEVICE_ID", 16},
};

const ina239_register_info_t *ina239_register_map(size_t *count)
{
    if (count) {
        *count = sizeof(s_registers) / sizeof(s_registers[0]);
    }
    return s_registers;
}
