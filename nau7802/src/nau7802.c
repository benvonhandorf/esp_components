/*
 * Nuvoton NAU7802 24-bit bridge ADC -- device layer.
 *
 * Unlike a command-byte part such as the SHT4x this is a conventional register
 * device, and it auto-increments on a burst read, so the three ADC result bytes
 * can be fetched in one transaction.
 *
 * See nau7802_priv.h for the register map and for the reasoning behind the
 * constants; see README.md for the summary.
 */
#include <math.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "nau7802_priv.h"

static const char *TAG = "nau7802";

/* GAINS[2:0] encodings, index = register value. */
static const int gain_values[8] = {1, 2, 4, 8, 16, 32, 64, 128};

/* CRS[2:0] encodings. Only these five are defined; 100-110 are unused. */
static const int rate_values[8] = {10, 20, 40, 80, -1, -1, -1, 320};

/* VLDO[2:0] encodings, in millivolts. */
static const int ldo_millivolts[8] = {4500, 4200, 3900, 3600, 3300, 3000, 2700, 2400};

const nau7802_gain_t nau7802_gains[8] = {
    NAU7802_GAIN_1,  NAU7802_GAIN_2,  NAU7802_GAIN_4,  NAU7802_GAIN_8,
    NAU7802_GAIN_16, NAU7802_GAIN_32, NAU7802_GAIN_64, NAU7802_GAIN_128,
};

const nau7802_rate_t nau7802_rates[5] = {
    NAU7802_RATE_10SPS, NAU7802_RATE_20SPS, NAU7802_RATE_40SPS,
    NAU7802_RATE_80SPS, NAU7802_RATE_320SPS,
};

const nau7802_ldo_t nau7802_ldos[8] = {
    NAU7802_LDO_4V5, NAU7802_LDO_4V2, NAU7802_LDO_3V9, NAU7802_LDO_3V6,
    NAU7802_LDO_3V3, NAU7802_LDO_3V0, NAU7802_LDO_2V7, NAU7802_LDO_2V4,
};

int nau7802_gain_value(nau7802_gain_t gain)
{
    return (gain >= 0 && gain < 8) ? gain_values[gain] : -1;
}

int nau7802_rate_sps(nau7802_rate_t rate)
{
    return (rate >= 0 && rate < 8) ? rate_values[rate] : -1;
}

int nau7802_ldo_millivolts(nau7802_ldo_t ldo)
{
    return (ldo >= 0 && ldo < 8) ? ldo_millivolts[ldo] : -1;
}

esp_err_t nau7802_gain_from_value(int value, nau7802_gain_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 8; i++) {
        if (gain_values[i] == value) {
            *out = (nau7802_gain_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t nau7802_rate_from_sps(int sps, nau7802_rate_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 8; i++) {
        if (rate_values[i] == sps) {
            *out = (nau7802_rate_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t nau7802_ldo_from_millivolts(int millivolts, nau7802_ldo_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 8; i++) {
        if (ldo_millivolts[i] == millivolts) {
            *out = (nau7802_ldo_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

/* ------------------------------------------------------------------ */
/* Transport                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t read_regs(nau7802_handle_t h, uint8_t reg, uint8_t *buffer, size_t len)
{
    if (!h->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(h->dev, &reg, 1, buffer, len, XFER_TIMEOUT_MS);
}

static esp_err_t read_reg(nau7802_handle_t h, uint8_t reg, uint8_t *value)
{
    return read_regs(h, reg, value, 1);
}

esp_err_t nau7802_priv_read_reg(nau7802_handle_t handle, uint8_t reg, uint8_t *value)
{
    return read_reg(handle, reg, value);
}

static esp_err_t write_reg(nau7802_handle_t h, uint8_t reg, uint8_t value)
{
    if (!h->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(h->dev, payload, sizeof(payload), XFER_TIMEOUT_MS);
}

static esp_err_t update_reg(nau7802_handle_t h, uint8_t reg, uint8_t clear_mask,
                            uint8_t set_mask)
{
    uint8_t value = 0;
    esp_err_t err = read_reg(h, reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    value = (uint8_t)((value & ~clear_mask) | set_mask);
    return write_reg(h, reg, value);
}

/* Read a big-endian multi-byte register into a host integer. */
static esp_err_t read_be(nau7802_handle_t h, uint8_t reg, size_t len, uint32_t *out)
{
    uint8_t raw[4];
    if (len > sizeof(raw)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = read_regs(h, reg, raw, len);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < len; i++) {
        value = (value << 8) | raw[i];
    }
    *out = value;
    return ESP_OK;
}

/* Wait for a status bit, polling on the FreeRTOS tick. */
static esp_err_t wait_for_bit(nau7802_handle_t h, uint8_t reg, uint8_t mask, bool set,
                              uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 10) {
        uint8_t value = 0;
        esp_err_t err = read_reg(h, reg, &value);
        if (err != ESP_OK) {
            return err;
        }
        if (((value & mask) != 0) == set) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
    }
    return ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* DRDY                                                               */
/* ------------------------------------------------------------------ */

/*
 * Whether this driver installed the shared GPIO ISR service.
 *
 * Process-wide rather than per-handle, because the service is. Installing it
 * twice returns ESP_ERR_INVALID_STATE, which is harmless in itself but logs at
 * error level on the way out -- so simply re-pointing DRDY at another pin would
 * emit an alarming line about a condition that is not a problem.
 */
static bool isr_service_ready;

static void IRAM_ATTR drdy_isr(void *arg)
{
    nau7802_handle_t h = (nau7802_handle_t)arg;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(h->drdy_signal, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* Stop using the pin and hand it back in its reset state. */
static void release_drdy(nau7802_handle_t h)
{
    if (h->drdy_gpio < 0) {
        return;
    }

    gpio_isr_handler_remove(h->drdy_gpio);
    gpio_set_intr_type(h->drdy_gpio, GPIO_INTR_DISABLE);
    gpio_reset_pin(h->drdy_gpio);
    h->drdy_gpio = -1;
}

static esp_err_t configure_drdy(nau7802_handle_t h, int pin)
{
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (h->drdy_signal == NULL) {
        h->drdy_signal = xSemaphoreCreateBinary();
        if (h->drdy_signal == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    release_drdy(h);

    /*
     * The device drives DRDY push-pull, so the pull-down is not there to hold
     * the line. It is there for the pin that turns out not to be connected to
     * it: a floating input can sit high and make every read look instantly
     * ready, which is indistinguishable from a working pin until the numbers
     * come out wrong. Pulled down, a wrong pin number times out and says so.
     */
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    if (!isr_service_ready) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK; /* something else in the application got there first */
        }
        if (err != ESP_OK) {
            return err;
        }
        isr_service_ready = true;
    }

    err = gpio_isr_handler_add(pin, drdy_isr, h);
    if (err != ESP_OK) {
        /* Leave no half-claimed pin behind: it is configured for an edge
         * nothing is listening for. */
        gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
        gpio_reset_pin(pin);
        return err;
    }

    h->drdy_gpio = pin;
    return ESP_OK;
}

/* Wait for the device to raise DRDY. Only called with a pin configured. */
static esp_err_t wait_for_drdy(nau7802_handle_t h, uint32_t timeout_ms)
{
    /*
     * Drop a signal left over from a conversion nobody read. Doing this before
     * sampling the level rather than after is what makes the sequence safe: an
     * edge arriving between the two still leaves the semaphore given, so the
     * take below returns immediately instead of missing the wake-up.
     */
    xSemaphoreTake(h->drdy_signal, 0);

    /*
     * Already high means a result is sitting in the registers unread, and there
     * will be no edge to wait for -- DRDY does not fall until the result is
     * read, so a conversion completing under a full register does not produce
     * one. Take it now. This read is the one case that keeps the old timing
     * risk, since its phase within the conversion is unknown; it can only
     * happen on the first sample of a batch, because reading the result drops
     * the line and re-arms the edge for every sample after it.
     */
    if (gpio_get_level(h->drdy_gpio) == 1) {
        return ESP_OK;
    }

    if (xSemaphoreTake(h->drdy_signal, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Conversions                                                        */
/* ------------------------------------------------------------------ */

/*
 * Take one conversion.
 *
 * The data sheet is explicit that reading ADCO without CR set latches and
 * shifts out the *previous* result, so readiness is established first --
 * otherwise an averaged read would silently be an average of one sample
 * repeated.
 */
static esp_err_t read_raw(nau7802_handle_t h, int32_t *out)
{
    esp_err_t err;

    if (h->drdy_gpio >= 0) {
        err = wait_for_drdy(h, CONVERSION_TIMEOUT_MS);
    } else {
        err = wait_for_bit(h, REG_PU_CTRL, PU_CTRL_CR, true, CONVERSION_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        return err;
    }

    /* Burst read: the NAU7802 auto-increments, so B2/B1/B0 come out in order. */
    uint8_t raw[3];
    err = read_regs(h, REG_ADCO_B2, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t value = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];

    /* Sign-extend the 24-bit two's complement result into int32_t. */
    if (value & 0x800000) {
        value |= 0xFF000000;
    }

    *out = (int32_t)value;
    return ESP_OK;
}

esp_err_t nau7802_read_raw(nau7802_handle_t handle, int32_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return read_raw(handle, out);
}

esp_err_t nau7802_read_average(nau7802_handle_t handle, int samples,
                               nau7802_stats_t *out)
{
    if (!handle || !out || samples < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    *out = (nau7802_stats_t){0};

    double total = 0.0;
    int32_t low = INT32_MAX;
    int32_t high = INT32_MIN;

    /* Welford, rather than accumulating a sum of squares: see the note on
     * nau7802_read_average() in the public header. */
    double running_mean = 0.0;
    double m2 = 0.0;

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        esp_err_t err = read_raw(handle, &value);
        if (err != ESP_OK) {
            /* Say how far it got: a timeout on sample 1 of 50 and one on
             * sample 49 are different faults. */
            out->samples = i;
            return err;
        }

        total += value;
        const double delta = value - running_mean;
        running_mean += delta / (i + 1);
        m2 += delta * (value - running_mean);

        if (value < low) {
            low = value;
        }
        if (value > high) {
            high = value;
        }
    }

    out->mean = total / samples;
    out->min = low;
    out->max = high;
    out->samples = samples;

    /*
     * A single conversion has no spread, so there is nothing to estimate a
     * noise figure from. Reporting 0 is *not* a claim of perfect knowledge --
     * callers that use the number must check the sample count and refuse,
     * because a zero uncertainty would let a calibration accept any change at
     * all, which is the exact failure that guard exists to prevent.
     */
    const double variance = samples > 1 ? m2 / (samples - 1) : 0.0;
    out->stderr_mean = samples > 1 ? sqrt(variance / samples) : 0.0;

    const double rail = ADC_SATURATION_FRACTION * NAU7802_FULL_SCALE;
    const double worst = fabs((double)low) > fabs((double)high) ? fabs((double)low)
                                                               : fabs((double)high);
    out->saturated = ((double)low <= -rail || (double)high >= rail);
    out->saturation_percent = 100.0 * worst / NAU7802_FULL_SCALE;

    return ESP_OK;
}

/* Run the internal offset calibration and report the device's own verdict. */
static esp_err_t run_calibration(nau7802_handle_t h)
{
    /*
     * CAL_ERR is cleared in the same write that starts the run, not left to the
     * read-modify-write to carry forward. The verdict below is a bit read back
     * out of the same register that reported the *previous* calibration, so a
     * stale 1 written back here would be indistinguishable from a fresh
     * failure -- one bad calibration would then condemn every one after it.
     */
    esp_err_t err = update_reg(h, REG_CTRL2,
                               CTRL2_CALMOD_MASK | CTRL2_CALS | CTRL2_CAL_ERR,
                               CALMOD_OFFSET_INTERNAL | CTRL2_CALS);
    if (err != ESP_OK) {
        return err;
    }

    /* CALS is an action bit: the device clears it when calibration completes. */
    err = wait_for_bit(h, REG_CTRL2, CTRL2_CALS, false, CALIBRATION_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t ctrl2 = 0;
    if (read_reg(h, REG_CTRL2, &ctrl2) == ESP_OK && (ctrl2 & CTRL2_CAL_ERR)) {
        return ESP_ERR_NAU7802_CAL_FAILED;
    }

    return ESP_OK;
}

/* Read and throw away `count` conversions. */
static esp_err_t discard_conversions(nau7802_handle_t h, int count)
{
    for (int i = 0; i < count; i++) {
        int32_t ignored = 0;
        esp_err_t err = read_raw(h, &ignored);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/*
 * Make sure conversions are running, and report whether they were not.
 *
 * Bring-up sets CS after calibrating, which reads as though calibration is
 * expected to leave the cycle stopped. Measured, it is not: PU_CTRL comes back
 * with CS set after every one of gain, rate and input, and this has yet to
 * report anything. Bring-up sets CS there because nothing had set it since the
 * register reset, not because CALS cleared it.
 *
 * Kept anyway. It is one transaction, it is a no-op on the silicon in front of
 * us, and it is the difference between "conversions stopped" being a hang and
 * being a line of output on some other board or revision.
 */
static esp_err_t restart_conversions(nau7802_handle_t h, bool *restarted)
{
    *restarted = false;

    uint8_t pu_ctrl = 0;
    esp_err_t err = read_reg(h, REG_PU_CTRL, &pu_ctrl);
    if (err != ESP_OK) {
        return err;
    }

    if (!(pu_ctrl & PU_CTRL_CS)) {
        err = write_reg(h, REG_PU_CTRL, (uint8_t)(pu_ctrl | PU_CTRL_CS));
        if (err != ESP_OK) {
            return err;
        }
        *restarted = true;
    }

    return ESP_OK;
}

/*
 * The one path every mid-run change to the analog path takes.
 *
 * Recalibrating is necessary but not sufficient. The device-side offset is
 * redone here; the conversions that predate the change are flushed, the filter
 * is given time to settle, and the *host*-side tare and scale -- which the
 * device knows nothing about -- are dropped, because they were measured at the
 * old gain and are wrong by exactly the factor nobody will notice. A gain
 * change after calibrating at x1 used to leave the weight reported as a
 * hundred and twenty-eighth of the true mass, self-consistently.
 *
 * Bring-up does not come through here: it has no conversions running to flush
 * and clears the scale itself.
 */
static esp_err_t apply_analog_change(nau7802_handle_t h, nau7802_change_report_t *report)
{
    nau7802_change_report_t local = {0};

    /*
     * First, and unconditionally. The caller has already written the register,
     * so the tare and scale are stale from this point on whatever happens next
     * -- and the paths that fail are exactly the ones where leaving a
     * plausible-looking scale factor behind would do the most damage.
     */
    local.scale_invalidated = h->scale.calibrated || h->scale.tare_samples > 0;
    /* Read before the reset, which is the only moment it is still knowable, and
     * reported because the advice a caller should give differs: a measured
     * factor is re-measured here, a supplied one has to come from a bench run
     * at the gain now in force. */
    local.scale_was_supplied = h->scale.supplied;
    nau7802_reset_scale(h);

    local.settling_conversions = SETTLING_CONVERSIONS;
    local.discards = STALE_CONVERSIONS + SETTLING_CONVERSIONS;

    esp_err_t err = run_calibration(h);
    if (err != ESP_OK) {
        local.failed_stage = NAU7802_STAGE_CALIBRATE;
    } else {
        err = restart_conversions(h, &local.conversions_restarted);
        if (err != ESP_OK) {
            local.failed_stage = NAU7802_STAGE_START;
        } else {
            err = discard_conversions(h, local.discards);
            if (err != ESP_OK) {
                local.failed_stage = NAU7802_STAGE_SETTLE;
            }
        }
    }

    if (report) {
        *report = local;
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

esp_err_t nau7802_create(const nau7802_config_t *config, nau7802_handle_t *out)
{
    if (!config || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    nau7802_handle_t h = calloc(1, sizeof(struct nau7802_dev_t));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }

    h->dev = config->dev;
    h->drdy_gpio = -1;

    if (config->drdy_gpio >= 0) {
        esp_err_t err = configure_drdy(h, config->drdy_gpio);
        if (err != ESP_OK) {
            if (h->drdy_signal) {
                vSemaphoreDelete(h->drdy_signal);
            }
            free(h);
            return err;
        }
    }

    *out = h;
    return ESP_OK;
}

esp_err_t nau7802_delete(nau7802_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    release_drdy(handle);
    if (handle->drdy_signal) {
        vSemaphoreDelete(handle->drdy_signal);
    }
    free(handle);
    return ESP_OK;
}

esp_err_t nau7802_set_device(nau7802_handle_t handle, i2c_master_dev_handle_t dev)
{
    if (!handle || !dev) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->dev = dev;
    return ESP_OK;
}

bool nau7802_is_ready(nau7802_handle_t handle)
{
    return handle && handle->ready;
}

esp_err_t nau7802_set_drdy(nau7802_handle_t handle, int gpio)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (gpio < 0) {
        release_drdy(handle);
        return ESP_OK;
    }
    return configure_drdy(handle, gpio);
}

int nau7802_drdy_gpio(nau7802_handle_t handle)
{
    return handle ? handle->drdy_gpio : -1;
}

int nau7802_drdy_level(nau7802_handle_t handle)
{
    if (!handle || handle->drdy_gpio < 0) {
        return -1;
    }
    return gpio_get_level(handle->drdy_gpio);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                           */
/* ------------------------------------------------------------------ */

#define BRINGUP_FAIL(stage_, err_)     \
    do {                               \
        local.failed_stage = (stage_); \
        handle->ready = false;         \
        if (report) {                  \
            *report = local;           \
        }                              \
        return (err_);                 \
    } while (0)

esp_err_t nau7802_bring_up(nau7802_handle_t handle, const nau7802_bringup_opts_t *opts,
                           nau7802_bringup_report_t *report)
{
    if (!handle || !opts) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Checked here rather than where it is installed, so a factor that could
     * never work is refused before the part is touched at all. The install
     * itself is on the success path at the bottom and has nowhere to report a
     * failure, and a compiled-in constant that divides every weight to infinity
     * is worth failing loudly for.
     */
    if (opts->set_scale &&
        (opts->counts_per_unit == 0.0 || !isfinite(opts->counts_per_unit))) {
        return ESP_ERR_INVALID_ARG;
    }

    nau7802_bringup_report_t local = {
        .failed_stage = NAU7802_STAGE_NONE,
        .gain = NAU7802_GAIN_1,
        .gain_was_requested = opts->set_gain,
        .ldo_enabled = opts->use_internal_ldo,
        .ldo = opts->ldo,
        .drdy_gpio = handle->drdy_gpio,
        .settling_discards = SETTLING_CONVERSIONS,
    };

    esp_err_t err = read_reg(handle, REG_DEVICE_REV, &local.revision);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_PROBE, err);
    }

    /* RR is level triggered: raise it to enter reset, drop it to leave. */
    err = write_reg(handle, REG_PU_CTRL, PU_CTRL_RR);
    if (err == ESP_OK) {
        err = write_reg(handle, REG_PU_CTRL, 0);
    }
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_RESET, err);
    }

    /* Digital first; PUA is only valid once PUD is set. */
    err = write_reg(handle, REG_PU_CTRL, PU_CTRL_PUD);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_POWER_DIGITAL, err);
    }

    err = wait_for_bit(handle, REG_PU_CTRL, PU_CTRL_PUR, true, CONVERSION_TIMEOUT_MS);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_POWER_READY, err);
    }

    /*
     * VLDO before AVDDS, and both before the analog section comes up.
     *
     * The register reset above returns CTRL1 to 0x00, and VLDO 000 is 4.5 V --
     * the top of the range. Setting AVDDS first therefore switched the internal
     * regulator on at 4.5 V and only then wound it down to whatever the board
     * asked for, putting an overvoltage on AVDD (and on anything the board
     * bridges from it to the load cell) for as long as one I2C transaction
     * takes. Configure the regulator while it is still off.
     */
    if (opts->use_internal_ldo) {
        err = update_reg(handle, REG_CTRL1, CTRL1_VLDO_MASK,
                         (uint8_t)(opts->ldo << CTRL1_VLDO_SHIFT));
        if (err != ESP_OK) {
            BRINGUP_FAIL(NAU7802_STAGE_SET_LDO, err);
        }
    }

    /* Before run_calibration() below, since the gain is part of the analog path
     * the offset calibration measures. */
    if (opts->set_gain) {
        err = update_reg(handle, REG_CTRL1, CTRL1_GAINS_MASK,
                         (uint8_t)(opts->gain << CTRL1_GAINS_SHIFT));
        if (err != ESP_OK) {
            BRINGUP_FAIL(NAU7802_STAGE_SET_GAIN, err);
        }
    }

    uint8_t pu_ctrl = PU_CTRL_PUD | PU_CTRL_PUA;
    if (opts->use_internal_ldo) {
        pu_ctrl |= PU_CTRL_AVDDS;
    }
    err = write_reg(handle, REG_PU_CTRL, pu_ctrl);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_POWER_ANALOG, err);
    }

    handle->ready = true;
    nau7802_reset_scale(handle);

    /*
     * Section 9.1 step 4b. Goes here, with the rest of the configuration and
     * before the settle delay and the calibration, because the calibration has
     * to measure the analog path the device will actually convert with.
     */
    err = write_reg(handle, REG_ADC_CTRL, ADC_CTRL_POWER_ON);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_ADC_CTRL, err);
    }

    /* AVDD is the reference the calibration is about to measure against, so it
     * has to have stopped moving first. */
    vTaskDelay(pdMS_TO_TICKS(ANALOG_SETTLE_MS));

    err = run_calibration(handle);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_CALIBRATE, err);
    }

    /* Begin continuous conversions. */
    err = update_reg(handle, REG_PU_CTRL, 0, PU_CTRL_CS);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_START, err);
    }

    /*
     * The filter starts with no history, so the first conversions out of a cold
     * start are as unsettled as the ones after a change. Nothing is stale here
     * -- there is no previous result to hold -- so only the settling count is
     * charged.
     */
    err = discard_conversions(handle, SETTLING_CONVERSIONS);
    if (err != ESP_OK) {
        BRINGUP_FAIL(NAU7802_STAGE_SETTLE, err);
    }

    /*
     * Read the gain and the chopper back rather than assuming them.
     *
     * Bring-up opens with a register reset, which returns GAINS to the chip
     * default of x1 -- so it silently undoes a gain set earlier. That is not a
     * visible failure: the converter still works, the offset calibration still
     * passes, and readings still look plausible. They are just 128 times
     * smaller. The chopper is the same shape of problem, worth six bits.
     */
    uint8_t ctrl1 = 0;
    if (read_reg(handle, REG_CTRL1, &ctrl1) == ESP_OK) {
        local.gain = (nau7802_gain_t)(ctrl1 & CTRL1_GAINS_MASK);
    } else if (opts->set_gain) {
        local.gain = opts->gain;
    }

    if (read_reg(handle, REG_ADC_CTRL, &local.adc_ctrl) == ESP_OK) {
        local.adc_ctrl_valid = true;
        local.chps = (uint8_t)((local.adc_ctrl & ADC_CHPS_MASK) >> ADC_CHPS_SHIFT);
        local.chopper_off = (local.chps == 3);
    }

    /*
     * Last, on the only path that gets here.
     *
     * Not up beside the nau7802_reset_scale() above: BRINGUP_FAIL clears
     * `ready` but leaves the scale alone, and five stages can still fail after
     * that point -- so installing the factor there would leave a handle
     * reporting a calibrated scale on a converter that never came up.
     */
    if (opts->set_scale) {
        handle->scale.counts_per_unit = opts->counts_per_unit;
        handle->scale.calibrated = true;
        handle->scale.supplied = true;
    }

    if (report) {
        *report = local;
    }
    ESP_LOGD(TAG, "ready, revision 0x%02X, gain x%d", local.revision,
             nau7802_gain_value(local.gain));
    return ESP_OK;
}

#undef BRINGUP_FAIL

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

/* Every setter shares this shape: write the register, then pay for the change. */
static esp_err_t set_field(nau7802_handle_t h, uint8_t reg, uint8_t clear, uint8_t set,
                           nau7802_change_report_t *report)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!h->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = update_reg(h, reg, clear, set);
    if (err != ESP_OK) {
        return err;
    }
    return apply_analog_change(h, report);
}

static esp_err_t get_reg_checked(nau7802_handle_t h, uint8_t reg, uint8_t *value)
{
    if (!h || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!h->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return read_reg(h, reg, value);
}

esp_err_t nau7802_get_gain(nau7802_handle_t handle, nau7802_gain_t *out)
{
    uint8_t ctrl1 = 0;
    esp_err_t err = get_reg_checked(handle, REG_CTRL1, &ctrl1);
    if (err != ESP_OK) {
        return err;
    }
    *out = (nau7802_gain_t)(ctrl1 & CTRL1_GAINS_MASK);
    return ESP_OK;
}

esp_err_t nau7802_set_gain(nau7802_handle_t handle, nau7802_gain_t gain,
                           nau7802_change_report_t *report)
{
    if (nau7802_gain_value(gain) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_field(handle, REG_CTRL1, CTRL1_GAINS_MASK,
                     (uint8_t)(gain << CTRL1_GAINS_SHIFT), report);
}

esp_err_t nau7802_get_rate(nau7802_handle_t handle, nau7802_rate_t *out)
{
    uint8_t ctrl2 = 0;
    esp_err_t err = get_reg_checked(handle, REG_CTRL2, &ctrl2);
    if (err != ESP_OK) {
        return err;
    }
    *out = (nau7802_rate_t)((ctrl2 & CTRL2_CRS_MASK) >> CTRL2_CRS_SHIFT);
    return ESP_OK;
}

esp_err_t nau7802_set_rate(nau7802_handle_t handle, nau7802_rate_t rate,
                           nau7802_change_report_t *report)
{
    if (nau7802_rate_sps(rate) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The conversion rate changes the modulator and filter settings, so the
     * existing offset calibration no longer applies. Skipping the recalibration
     * leaves the ADC returning values that swing across most of full scale. */
    return set_field(handle, REG_CTRL2, CTRL2_CRS_MASK,
                     (uint8_t)(rate << CTRL2_CRS_SHIFT), report);
}

esp_err_t nau7802_get_channel(nau7802_handle_t handle, nau7802_channel_t *out)
{
    uint8_t ctrl2 = 0;
    esp_err_t err = get_reg_checked(handle, REG_CTRL2, &ctrl2);
    if (err != ESP_OK) {
        return err;
    }
    *out = (ctrl2 & CTRL2_CHS) ? NAU7802_CHANNEL_B : NAU7802_CHANNEL_A;
    return ESP_OK;
}

esp_err_t nau7802_set_channel(nau7802_handle_t handle, nau7802_channel_t channel,
                              nau7802_change_report_t *report)
{
    return set_field(handle, REG_CTRL2, CTRL2_CHS,
                     channel == NAU7802_CHANNEL_B ? CTRL2_CHS : 0, report);
}

esp_err_t nau7802_get_ldomode(nau7802_handle_t handle, bool *out)
{
    uint8_t pga = 0;
    esp_err_t err = get_reg_checked(handle, REG_PGA, &pga);
    if (err != ESP_OK) {
        return err;
    }
    *out = (pga & PGA_LDOMODE) != 0;
    return ESP_OK;
}

esp_err_t nau7802_set_ldomode(nau7802_handle_t handle, bool stable,
                              nau7802_change_report_t *report)
{
    /* The two modes settle AVDD at slightly different levels, and AVDD is the
     * reference, so the existing offset calibration no longer applies. */
    return set_field(handle, REG_PGA, PGA_LDOMODE, stable ? PGA_LDOMODE : 0, report);
}

esp_err_t nau7802_get_pga_cap(nau7802_handle_t handle, bool *out)
{
    uint8_t power = 0;
    esp_err_t err = get_reg_checked(handle, REG_POWER_CTRL, &power);
    if (err != ESP_OK) {
        return err;
    }
    *out = (power & POWER_PGA_CAP_EN) != 0;
    return ESP_OK;
}

esp_err_t nau7802_set_pga_cap(nau7802_handle_t handle, bool enable,
                              nau7802_change_report_t *report)
{
    return set_field(handle, REG_POWER_CTRL, POWER_PGA_CAP_EN,
                     enable ? POWER_PGA_CAP_EN : 0, report);
}

/* ------------------------------------------------------------------ */
/* Status and diagnostics                                             */
/* ------------------------------------------------------------------ */

/*
 * Decode what the internal calibration chose for the selected channel.
 *
 * OCAL is sign-and-magnitude, not two's complement -- bit 23 is the sign and
 * the low 23 bits are the magnitude. The result registers a few addresses away
 * *are* two's complement, which is what makes this worth spelling out: decoding
 * OCAL the same way reads every negative offset as a number pinned within a
 * couple of thousand counts of negative full scale.
 *
 * Measured across six calibrations: 0x000F58, 0x800622, 0x80000C, 0x001D82,
 * 0x000792, 0x8006F9. As sign-magnitude those are +3928, -1570, -12, +7554,
 * +1938, -1785 -- small offsets scattered either side of zero, which is what an
 * offset calibration produces. As two's complement the three with bit 23 set
 * would all be about -8.38 million, i.e. three independent calibrations landing
 * on the rail with the device still reporting CAL_ERR clear. The data sheet
 * does not spell the encoding out; the silicon does.
 *
 * Worth reading at all because CAL_ERR is one bit and only says the device gave
 * up. The offset it settled on is the number that says whether the front end is
 * anywhere near balanced: an OCAL close to zero means the bridge sits near
 * mid-supply, one up against the rails means it does not and the gain has
 * nowhere to go.
 */
static void read_calibration_regs(nau7802_handle_t h, nau7802_status_t *out)
{
    const bool channel_b = (out->channel == NAU7802_CHANNEL_B);
    const uint8_t ocal_reg = channel_b ? REG_OCAL2_B2 : REG_OCAL1_B2;
    const uint8_t gcal_reg = channel_b ? REG_GCAL2_B3 : REG_GCAL1_B3;

    uint32_t ocal = 0, gcal = 0;
    if (read_be(h, ocal_reg, 3, &ocal) != ESP_OK ||
        read_be(h, gcal_reg, 4, &gcal) != ESP_OK) {
        out->cal_regs_valid = false;
        return;
    }

    const int32_t magnitude = (int32_t)(ocal & 0x7FFFFF);

    out->cal_regs_valid = true;
    out->ocal_raw = ocal;
    out->ocal_counts = (ocal & 0x800000) ? -magnitude : magnitude;
    out->gcal_raw = gcal;
    out->gcal_ratio = gcal / GCAL_UNITY;
}

esp_err_t nau7802_get_status(nau7802_handle_t handle, nau7802_status_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (nau7802_status_t){0};

    esp_err_t err = read_reg(handle, REG_PU_CTRL, &out->pu_ctrl);
    if (err == ESP_OK) {
        err = read_reg(handle, REG_CTRL1, &out->ctrl1);
    }
    if (err == ESP_OK) {
        err = read_reg(handle, REG_CTRL2, &out->ctrl2);
    }
    if (err == ESP_OK) {
        err = read_reg(handle, REG_DEVICE_REV, &out->revision);
    }
    if (err != ESP_OK) {
        return err;
    }

    out->digital_up = (out->pu_ctrl & PU_CTRL_PUD) != 0;
    out->analog_up = (out->pu_ctrl & PU_CTRL_PUA) != 0;
    out->power_ready = (out->pu_ctrl & PU_CTRL_PUR) != 0;
    out->data_ready = (out->pu_ctrl & PU_CTRL_CR) != 0;
    out->conversions_started = (out->pu_ctrl & PU_CTRL_CS) != 0;
    out->internal_ldo = (out->pu_ctrl & PU_CTRL_AVDDS) != 0;

    out->gain = (nau7802_gain_t)(out->ctrl1 & CTRL1_GAINS_MASK);
    out->ldo = (nau7802_ldo_t)((out->ctrl1 & CTRL1_VLDO_MASK) >> CTRL1_VLDO_SHIFT);
    out->rate = (nau7802_rate_t)((out->ctrl2 & CTRL2_CRS_MASK) >> CTRL2_CRS_SHIFT);
    out->rate_sps = nau7802_rate_sps(out->rate);
    out->channel = (out->ctrl2 & CTRL2_CHS) ? NAU7802_CHANNEL_B : NAU7802_CHANNEL_A;
    out->cal_error = (out->ctrl2 & CTRL2_CAL_ERR) != 0;

    read_calibration_regs(handle, out);

    if (read_reg(handle, REG_PGA, &out->pga) == ESP_OK &&
        read_reg(handle, REG_POWER_CTRL, &out->power) == ESP_OK) {
        out->pga_valid = true;
        out->ldomode = (out->pga & PGA_LDOMODE) != 0;
        out->pga_cap = (out->power & POWER_PGA_CAP_EN) != 0;
    }

    /*
     * REG0x15 is worth decoding here rather than leaving it to a register dump:
     * a chopper left at the power-up 00 is the difference between about twelve
     * and about eighteen effective bits, and nothing else in this output would
     * show it. Valid to read because REG0x1B[7] RD_OTP_SEL is 0; were it 1,
     * this address would return OTP[32:24] instead.
     */
    if (read_reg(handle, REG_ADC_CTRL, &out->adc_ctrl) == ESP_OK) {
        out->adc_ctrl_valid = true;
        out->chps = (uint8_t)((out->adc_ctrl & ADC_CHPS_MASK) >> ADC_CHPS_SHIFT);
        out->chopper_off = (out->chps == 3);
    }

    out->drdy_gpio = handle->drdy_gpio;
    out->drdy_level = nau7802_drdy_level(handle);
    out->brought_up = handle->ready;
    out->scale = handle->scale;

    return ESP_OK;
}

esp_err_t nau7802_read_register(nau7802_handle_t handle, uint8_t reg, uint8_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_reg(handle, reg, out);
}

/*
 * The whole map, not 0x00-0x0B.
 *
 * That old range stopped in the middle of channel 2's offset calibration -- so
 * a dump covered part of one calibration block, none of the other, and neither
 * the result registers nor PGA/POWER. During bringup those are the registers
 * worth seeing. 0x15 is included as a read; the caution about it is against
 * *writing* it.
 */
static const nau7802_register_info_t register_map[] = {
    {0x00, "PU_CTRL"},  {0x01, "CTRL1"},    {0x02, "CTRL2"},
    {0x03, "OCAL1_B2"}, {0x04, "OCAL1_B1"}, {0x05, "OCAL1_B0"},
    {0x06, "GCAL1_B3"}, {0x07, "GCAL1_B2"}, {0x08, "GCAL1_B1"},
    {0x09, "GCAL1_B0"},
    {0x0A, "OCAL2_B2"}, {0x0B, "OCAL2_B1"}, {0x0C, "OCAL2_B0"},
    {0x0D, "GCAL2_B3"}, {0x0E, "GCAL2_B2"}, {0x0F, "GCAL2_B1"},
    {0x10, "GCAL2_B0"},
    {0x11, "I2C_CTRL"},
    {0x12, "ADCO_B2"},  {0x13, "ADCO_B1"},  {0x14, "ADCO_B0"},
    {REG_ADC_CTRL, "ADC/OTP"},
    {REG_PGA, "PGA"},   {REG_POWER_CTRL, "POWER"},
    {REG_DEVICE_REV, "DEVICE_REV"},
};

const nau7802_register_info_t *nau7802_register_map(size_t *count)
{
    if (count) {
        *count = sizeof(register_map) / sizeof(register_map[0]);
    }
    return register_map;
}
