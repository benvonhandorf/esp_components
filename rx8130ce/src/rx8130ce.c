#include "rx8130ce.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rx8130ce_calc.h"

#define REG_SEC   0x10
#define REG_FLAG  0x1D
#define REG_CTRL0 0x1E

/* Voltage-low flags: the part sets these when its supply and backup have both
 * fallen far enough to lose the count. */
#define FLAG_VLF  (1 << 1)
#define FLAG_VBLF (1 << 2)

#define I2C_TIMEOUT_MS 1000

struct rx8130ce_dev_t {
    i2c_master_dev_handle_t dev;
    bool power_lost;
};

static esp_err_t read_regs(struct rx8130ce_dev_t *d, uint8_t reg, uint8_t *buf, size_t n)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(d->dev, &reg, 1, buf, n, I2C_TIMEOUT_MS);
}

static esp_err_t write_regs(struct rx8130ce_dev_t *d, uint8_t reg,
                            const uint8_t *buf, size_t n)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;
    uint8_t tmp[1 + RX8130CE_TIME_REGS];
    if (n > RX8130CE_TIME_REGS) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(tmp + 1, buf, n);
    return i2c_master_transmit(d->dev, tmp, n + 1, I2C_TIMEOUT_MS);
}

static esp_err_t probe(struct rx8130ce_dev_t *d, rx8130ce_report_t *report)
{
    if (!d->dev) return ESP_ERR_INVALID_STATE;

    uint8_t flag = 0;
    esp_err_t err = read_regs(d, REG_FLAG, &flag, 1);
    if (err != ESP_OK) return err;

    d->power_lost = (flag & (FLAG_VLF | FLAG_VBLF)) != 0;
    if (report) {
        report->flag_register = flag;
        report->power_lost = d->power_lost;
    }
    return ESP_OK;
}

esp_err_t rx8130ce_create(const rx8130ce_config_t *cfg, rx8130ce_handle_t *out,
                          rx8130ce_report_t *report)
{
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (report) memset(report, 0, sizeof(*report));

    struct rx8130ce_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->dev = cfg->dev;

    if (d->dev) {
        esp_err_t err = probe(d, report);
        if (err != ESP_OK) {
            free(d);
            return err;
        }
    }

    *out = d;
    return ESP_OK;
}

void rx8130ce_delete(rx8130ce_handle_t handle) { free(handle); }

esp_err_t rx8130ce_set_device(rx8130ce_handle_t handle, i2c_master_dev_handle_t dev,
                              rx8130ce_report_t *report)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (report) memset(report, 0, sizeof(*report));
    handle->dev = dev;
    return dev ? probe(handle, report) : ESP_OK;
}

esp_err_t rx8130ce_get_time(rx8130ce_handle_t handle, struct timeval *out)
{
    if (!handle || !out) return ESP_ERR_INVALID_ARG;

    uint8_t regs[RX8130CE_TIME_REGS];
    esp_err_t err = read_regs(handle, REG_SEC, regs, sizeof(regs));
    if (err != ESP_OK) return err;

    struct tm t;
    if (!rx8130ce_decode_time(regs, &t)) {
        /* The part is present and answering; it just does not hold a valid time.
         * Distinct from an I2C error, because the responses differ: one needs a
         * bus fixed, the other needs the clock set. */
        return ESP_ERR_RX8130CE_NOT_SET;
    }

    /* timegm, not mktime: the RTC holds UTC, and mktime would apply whatever
     * timezone happens to be set and shift the result. */
    time_t seconds = timegm(&t);
    if (seconds == (time_t)-1) {
        return ESP_ERR_RX8130CE_NOT_SET;
    }

    out->tv_sec = seconds;
    out->tv_usec = 0;   /* the part has no sub-second resolution to read */
    return ESP_OK;
}

esp_err_t rx8130ce_set_time(rx8130ce_handle_t handle, const struct timeval *tv)
{
    if (!handle || !tv) return ESP_ERR_INVALID_ARG;

    struct tm t;
    time_t seconds = tv->tv_sec;
    if (!gmtime_r(&seconds, &t)) {
        return ESP_ERR_RX8130CE_BAD_TIME;
    }

    uint8_t regs[RX8130CE_TIME_REGS];
    if (!rx8130ce_encode_time(&t, regs)) {
        return ESP_ERR_RX8130CE_BAD_TIME;
    }

    esp_err_t err = write_regs(handle, REG_SEC, regs, sizeof(regs));
    if (err != ESP_OK) return err;

    /* Clear the voltage-low flags: the time is now known, and leaving them set
     * would make every later read claim the clock is unreliable. */
    uint8_t flag = 0;
    err = read_regs(handle, REG_FLAG, &flag, 1);
    if (err == ESP_OK) {
        flag &= (uint8_t)~(FLAG_VLF | FLAG_VBLF);
        err = write_regs(handle, REG_FLAG, &flag, 1);
    }
    if (err == ESP_OK) {
        handle->power_lost = false;
    }
    return err;
}

bool rx8130ce_power_was_lost(rx8130ce_handle_t handle)
{
    return handle ? handle->power_lost : false;
}
