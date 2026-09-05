/*
 * Tare, calibration and weighing for the HX711.
 *
 * This is arithmetic above the converter, not device access: everything here
 * goes through hx711_priv_read_average(). It mirrors
 * components/nau7802/src/nau7802_scale.c, because the maths above the converter
 * is the same on both parts and two drivers that disagreed about the shape of
 * it would be two things to learn.
 */
#include "hx711_priv.h"

#include <math.h>

void hx711_reset_scale(hx711_handle_t handle)
{
    if (handle) {
        handle->scale = (hx711_scale_t){0};
    }
}

const hx711_scale_t *hx711_get_scale(hx711_handle_t handle)
{
    return handle ? &handle->scale : NULL;
}

esp_err_t hx711_set_scale(hx711_handle_t handle, double counts_per_unit)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Zero divides every weight to infinity and a NaN propagates through the
     * lot without raising anything, so neither can reach the weighing code. A
     * negative factor is not in that class: a cell wired the other way round
     * genuinely calibrates to one, and the arithmetic already divides by the
     * signed value while taking fabs() for the uncertainty denominator.
     */
    if (counts_per_unit == 0.0 || !isfinite(counts_per_unit)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Bring-up power-cycles the part and clears this state on the way through,
     * so a factor set before it would be silently gone rather than in force.
     * Refusing is more use than losing it; hx711_bringup_opts_t is how a factor
     * gets applied at the right moment.
     */
    if (!handle->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    handle->scale.counts_per_unit = counts_per_unit;
    handle->scale.calibrated = true;
    handle->scale.supplied = true;
    return ESP_OK;
}

esp_err_t hx711_tare(hx711_handle_t handle, int samples, hx711_stats_t *stats)
{
    if (!handle || !stats) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = hx711_priv_read_average(handle, samples, stats);
    if (err != ESP_OK) {
        return err;
    }

    handle->scale.tare_counts = (int32_t)stats->mean;
    handle->scale.tare_stderr = stats->stderr_mean;
    handle->scale.tare_samples = samples;
    return ESP_OK;
}

esp_err_t hx711_calibrate(hx711_handle_t handle, double known_mass, int samples,
                          hx711_stats_t *stats, hx711_calibration_t *result)
{
    if (!handle || !stats || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (known_mass == 0.0 || !isfinite(known_mass)) {
        return ESP_ERR_INVALID_ARG;
    }

    *result = (hx711_calibration_t){0};
    result->mode = handle->mode;

    esp_err_t err = hx711_priv_read_average(handle, samples, stats);
    if (err != ESP_OK) {
        return err;
    }

    const double net = stats->mean - handle->scale.tare_counts;
    result->net_counts = net;

    /*
     * One conversion yields no spread, so neither batch can report an honest
     * uncertainty and the guard below would be comparing against zero -- which
     * accepts anything. Refuse rather than calibrate on a number that cannot be
     * checked.
     */
    if (samples < 2 || handle->scale.tare_samples < 2) {
        return ESP_ERR_HX711_TOO_FEW_SAMPLES;
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
     * on the sensor board: at 4765 counts RMS the old guard demanded 147k
     * counts at 10 samples and 262k at 200, against a 100 g signal of ~107k.
     *
     * Both averages carry error, so they add in quadrature; the tare's is kept
     * from when it ran.
     */
    const double uncertainty = hypot(stats->stderr_mean, handle->scale.tare_stderr);
    result->uncertainty = uncertainty;

    if (fabs(net) < HX711_CALIBRATION_SIGMA * uncertainty) {
        return ESP_ERR_HX711_WITHIN_NOISE;
    }

    handle->scale.counts_per_unit = net / known_mass;
    handle->scale.calibrated = true;
    /* A measurement made here replaces a supplied factor, and the provenance
     * has to move with the number or a caller reports the previous truth. */
    handle->scale.supplied = false;

    result->counts_per_unit = handle->scale.counts_per_unit;
    /*
     * Report the precision rather than only the number. The scale factor is a
     * ratio of a measured difference to a stated mass, so its relative error is
     * the relative error of that difference -- and every weight from here
     * inherits it.
     */
    result->precision_percent = 100.0 * uncertainty / fabs(net);
    return ESP_OK;
}

esp_err_t hx711_stats_to_weight(hx711_handle_t handle, const hx711_stats_t *in,
                                hx711_weight_t *out)
{
    if (!handle || !in || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->scale.calibrated) {
        return ESP_ERR_INVALID_STATE;
    }

    *out = (hx711_weight_t){0};
    out->tare_taken = handle->scale.tare_samples > 0;

    const double net = in->mean - handle->scale.tare_counts;
    out->net_counts = net;
    out->units = net / handle->scale.counts_per_unit;

    /*
     * Two different numbers, and reporting only one of them misleads.
     *
     * The +/- belongs to the figure actually reported, which is a mean of
     * `samples` conversions -- so it is the standard error of that mean,
     * combined with the tare's, not the peak-to-peak spread. Quoting the spread
     * overstates the error of the reported number by roughly sqrt(samples).
     *
     * The spread is still worth having, because during bringup it answers a
     * different question -- what a single reading would look like, and whether
     * the front end is behaving -- so it goes alongside rather than instead.
     */
    const double magnitude = fabs(handle->scale.counts_per_unit);
    out->uncertainty_units =
        in->samples > 1 ? hypot(in->stderr_mean, handle->scale.tare_stderr) / magnitude
                        : 0.0;
    out->spread_units = (double)(in->max - in->min) / magnitude;
    return ESP_OK;
}

esp_err_t hx711_weigh(hx711_handle_t handle, int samples, hx711_stats_t *stats,
                      hx711_weight_t *out)
{
    if (!handle || !stats || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->scale.calibrated) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = hx711_priv_read_average(handle, samples, stats);
    if (err != ESP_OK) {
        return err;
    }

    return hx711_stats_to_weight(handle, stats, out);
}
