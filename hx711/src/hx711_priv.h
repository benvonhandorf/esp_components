/*
 * HX711 timing constants and handle state. Not on the include path -- this
 * header is private to the component.
 */
#ifndef HX711_PRIV_H
#define HX711_PRIV_H

#include "hx711.h"

#include "freertos/FreeRTOS.h"

/*
 * Table 3, Input Channel and Gain Selection. Channel B's gain is fixed at 32
 * and channel A cannot be set to it, so gain and channel are two views of one
 * setting.
 */
#define HX711_PULSES_A128 25
#define HX711_PULSES_B32  26
#define HX711_PULSES_A64  27

/* Figure 2's timing table. T3's maximum and the power-down threshold are
 * public; see hx711.h for the gap between them. */
#define HX711_SCK_HIGH_US 1
#define HX711_SCK_LOW_US  1

/* Comfortably past 60 us, and clear of the undefined 50-60 us window. */
#define HX711_POWERDOWN_HOLD_US 100

/*
 * Table 2 gives the output settling time -- explicitly "from power up, reset,
 * input channel change and gain change to valid stable output data" -- as
 * 400 ms with RATE low and 50 ms with RATE high. Those are 10 SPS and 80 SPS
 * respectively, so both work out to exactly four output periods.
 *
 * Counting conversions rather than milliseconds is what lets this driver settle
 * correctly without knowing which way the RATE strap is wired, which it has no
 * way to ask.
 */
/*
 * UNVERIFIED. Figure 2 labels the trailing pulses "Next Conversion", which
 * reads as the following conversion already using the new setting, but the data
 * sheet never says outright whether a conversion already under way when those
 * pulses land uses the old selection or the new one. One extra discard covers
 * the pessimistic reading and costs 100 ms at 10 SPS.
 */
#define HX711_STALE_CONVERSIONS 1

/* HX711_SETTLING_CONVERSIONS and HX711_CHANGE_DISCARDS are public; see hx711.h.
 * The identity below is what lets a caller announce the wait before it starts. */
_Static_assert(HX711_CHANGE_DISCARDS ==
                   HX711_STALE_CONVERSIONS + HX711_SETTLING_CONVERSIONS,
               "the published discard count must match what apply_mode() does");

/*
 * Ten conversion periods at RATE low, the rate almost every breakout is
 * strapped to, and eighty at RATE high.
 */
#define HX711_READY_TIMEOUT_MS 1000
#define HX711_READY_POLL_US    20

/* Enough conversions to time, bounded so 80 SPS does not spend a whole second
 * proving it is 80 SPS. See measure_rate(). */
#define RATE_MIN_CONVERSIONS 4
#define RATE_MAX_CONVERSIONS 16
#define RATE_BUDGET_US       300000

/*
 * Table 2 gives 10 and 80 SPS as bare typical values -- the internal oscillator
 * has no tolerance column filled in at all -- so real parts land wherever they
 * land. These bands are deliberately wide; the two rates are 8x apart, so there
 * is nothing to gain from tight ones and something to lose.
 */
#define RATE_10SPS_LO   6.0
#define RATE_10SPS_HI  16.0
#define RATE_80SPS_LO  48.0
#define RATE_80SPS_HI 130.0

/*
 * Ten times the combined uncertainty of the two averages is a scale factor good
 * to 10%, which is the point below which the number is not worth having.
 */
#define HX711_CALIBRATION_SIGMA 10.0

struct hx711_dev_t {
    bool ready;
    bool powered_down;
    int dout_pin;
    int sck_pin;

    /* The reset default, per "Reset and Power-Down". */
    hx711_mode_t mode;
    /* Which channel A gain to return to when channel A follows channel B; the
     * channel selection alone does not say which of the two A gains was meant. */
    hx711_mode_t last_a_mode;

    /*
     * Tare and scale in one struct rather than five fields.
     *
     * Three places clear this state -- a gain change, bring-up and delete --
     * and as loose fields that was five assignments in each, which is five
     * chances for a new one to be cleared in two places out of three.
     */
    hx711_scale_t scale;

    /* 0 until hx711_measure_rate() has run. */
    double measured_sps;

    /* Worst PD_SCK high phase seen since bring-up, and how many bursts broke
     * T3. Reported by status because a driver that has silently retried is
     * worth knowing about even when every reading it returned was good. */
    uint32_t worst_high_us;
    uint32_t stretched_bursts;
};

/* Shared with hx711_scale.c. */
esp_err_t hx711_priv_read_average(hx711_handle_t handle, int samples,
                                  hx711_stats_t *stats);

#endif
