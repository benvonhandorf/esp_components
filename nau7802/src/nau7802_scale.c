/*
 * NAU7802 -- host-side scale arithmetic.
 *
 * The device's internal calibration handles its own offset in ADC counts. This
 * is the layer above: a tare captured with the cell empty, a scale factor
 * derived from a known mass, and the uncertainty that both carry into every
 * weight reported afterwards.
 *
 * None of this touches a NAU7802 register except to read the gain back when a
 * calibration is refused, so it is kept in its own file -- the arithmetic is
 * the same for any load cell front end, and one day it may want to be shared
 * with one.
 */
#include <math.h>

#include "nau7802_priv.h"

void nau7802_reset_scale(nau7802_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->scale = (nau7802_scale_t){0};
}

const nau7802_scale_t *nau7802_get_scale(nau7802_handle_t handle)
{
    return handle ? &handle->scale : NULL;
}

esp_err_t nau7802_set_scale(nau7802_handle_t handle, double counts_per_unit)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Zero divides every weight to infinity and a NaN propagates through the
     * lot without ever raising anything, so neither can be allowed to reach
     * nau7802_weigh(). A negative factor is not in that class: a cell wired the
     * other way round genuinely calibrates to one, and the arithmetic below
     * already divides by the signed value while taking fabs() for the
     * uncertainty denominator.
     */
    if (counts_per_unit == 0.0 || !isfinite(counts_per_unit)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Bring-up drops the scale, so a factor set before it would not survive to
     * be used. Refusing is more use than silently losing it later. */
    if (!handle->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    handle->scale.counts_per_unit = counts_per_unit;
    handle->scale.calibrated = true;
    handle->scale.supplied = true;
    return ESP_OK;
}

esp_err_t nau7802_tare(nau7802_handle_t handle, int samples, nau7802_stats_t *stats)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    nau7802_stats_t local = {0};
    esp_err_t err = nau7802_read_average(handle, samples, &local);
    if (stats) {
        *stats = local;
    }
    if (err != ESP_OK) {
        return err;
    }

    handle->scale.tare_counts = local.mean;
    handle->scale.tare_stderr = local.stderr_mean;
    handle->scale.tare_samples = local.samples;
    return ESP_OK;
}

esp_err_t nau7802_calibrate(nau7802_handle_t handle, double known_mass, int samples,
                            nau7802_stats_t *stats, nau7802_calibration_t *result)
{
    if (!handle || known_mass == 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    nau7802_stats_t local_stats = {0};
    nau7802_calibration_t local = {0};

    esp_err_t err = nau7802_read_average(handle, samples, &local_stats);
    if (stats) {
        *stats = local_stats;
    }
    if (err != ESP_OK) {
        if (result) {
            *result = local;
        }
        return err;
    }

    local.net_counts = local_stats.mean - handle->scale.tare_counts;
    local.uncertainty = hypot(local_stats.stderr_mean, handle->scale.tare_stderr);

    /*
     * Read the gain back whether or not this succeeds. When a calibration is
     * refused, a PGA turned down is the one cause that averaging cannot fix,
     * and nothing about the numbers points at it.
     */
    uint8_t ctrl1 = 0;
    if (nau7802_priv_read_reg(handle, REG_CTRL1, &ctrl1) == ESP_OK) {
        local.gain = (nau7802_gain_t)(ctrl1 & CTRL1_GAINS_MASK);
        local.gain_valid = true;
    }

    /*
     * One conversion yields no spread, so neither batch can report an honest
     * uncertainty and the guard below would be comparing against zero -- which
     * accepts anything. Refuse rather than calibrate on a number that cannot be
     * checked.
     */
    if (local_stats.samples < 2 || handle->scale.tare_samples < 2) {
        if (result) {
            *result = local;
        }
        return ESP_ERR_NAU7802_TOO_FEW_SAMPLES;
    }

    /*
     * Reject a calibration the noise could have produced.
     *
     * The test is on the uncertainty of the two *averages* being subtracted,
     * not on the spread of the individual samples, and the difference matters
     * more than it looks. Peak-to-peak spread grows with the sample count --
     * more draws, more chance of an extreme one -- while the uncertainty of a
     * mean falls as 1/sqrt(n). A guard written against the spread therefore
     * gets harder to satisfy the more you average, so "take more samples", the
     * one correct response to a noisy part, made the refusal worse instead of
     * better and put calibration out of reach at every sample count. Measured
     * on a board with a 4765-counts-RMS front end: the old guard demanded 147k
     * counts at 10 samples and 262k at 200, against a 100 g signal of ~107k.
     *
     * Both averages carry error, so they add in quadrature; the tare's is kept
     * from when it ran.
     */
    if (fabs(local.net_counts) < CALIBRATION_SIGMA * local.uncertainty) {
        if (result) {
            *result = local;
        }
        return ESP_ERR_NAU7802_WITHIN_NOISE;
    }

    local.counts_per_unit = local.net_counts / known_mass;

    /*
     * The scale factor is a ratio of a measured difference to a stated mass, so
     * its relative error is the relative error of that difference -- and every
     * weight from here inherits it.
     */
    local.precision_percent = 100.0 * local.uncertainty / fabs(local.net_counts);

    handle->scale.counts_per_unit = local.counts_per_unit;
    handle->scale.calibrated = true;
    /* A measurement made here replaces a supplied factor, and the provenance
     * has to move with the number or `status` reports the previous truth. */
    handle->scale.supplied = false;

    if (result) {
        *result = local;
    }
    return ESP_OK;
}

esp_err_t nau7802_weigh(nau7802_handle_t handle, int samples, nau7802_stats_t *stats,
                        nau7802_weight_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->scale.calibrated) {
        return ESP_ERR_INVALID_STATE;
    }

    nau7802_stats_t local = {0};
    esp_err_t err = nau7802_read_average(handle, samples, &local);
    if (stats) {
        *stats = local;
    }
    if (err != ESP_OK) {
        return err;
    }

    const double scale = fabs(handle->scale.counts_per_unit);

    *out = (nau7802_weight_t){0};
    out->net_counts = local.mean - handle->scale.tare_counts;
    out->units = out->net_counts / handle->scale.counts_per_unit;

    /*
     * A supplied factor can make `calibrated` true with no tare behind it, and
     * subtracting a zero tare turns the bridge's own offset into a large,
     * confident, entirely fictional load. The reading is still returned -- the
     * caller is the one placed to say so -- but it is flagged.
     */
    out->tare_taken = handle->scale.tare_samples > 0;

    /*
     * Two different numbers, and reporting only one of them misleads.
     *
     * The uncertainty belongs to the figure actually produced, which is a mean
     * of `samples` conversions -- so it is the standard error of that mean,
     * combined with the tare's, not the peak-to-peak spread. Quoting the spread
     * overstates the error of the printed number by roughly sqrt(samples).
     *
     * The spread is still worth having, because during bringup it answers a
     * different question -- what a single reading would look like, and whether
     * the front end is behaving -- so it goes alongside rather than instead.
     *
     * One conversion has no spread at all, so both come back zero and the
     * caller is expected to quote no error bar rather than a zero one.
     */
    if (local.samples > 1) {
        out->uncertainty_units =
            hypot(local.stderr_mean, handle->scale.tare_stderr) / scale;
        out->spread_units = (local.max - local.min) / scale;
    }

    return ESP_OK;
}
