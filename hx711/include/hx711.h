/*
 * Avia Semiconductor HX711 24-bit ADC for weigh scales, driven as a load cell
 * front end.
 *
 * Pulse counts, timings and encodings come from the HX711 data sheet as
 * published at
 * https://cdn.sparkfun.com/datasheets/Sensors/ForceFlex/hx711_english.pdf
 * (retrieved 2026-08-26). That document carries no revision number or date
 * anywhere, so it can only be cited by source. The few constants it does not
 * actually state are marked UNVERIFIED where they are defined.
 *
 * The part has no control bus and no registers. Two pins carry everything: the
 * device pulls DOUT low when a conversion is ready, and the host clocks 24 bits
 * out of it on PD_SCK -- then keeps clocking, because the *number* of extra
 * pulses is how the next conversion's channel and gain are selected. So this
 * driver's whole configuration surface is a pulse count and a pin level.
 *
 * This driver formats no text. Calls return esp_err_t and fill out-structs with
 * facts -- which stage of the bring-up failed, whether a clock pulse broke the
 * data sheet's timing limit, whether a calibration move was inside the noise --
 * so the caller can report them in whatever way suits it.
 */
#ifndef HX711_H
#define HX711_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Table 2: the output is two's complement, saturating at 0x800000 (min) and
 * 0x7FFFFF (max), so the full 24-bit range is available.
 */
#define HX711_FULL_SCALE 8388608.0

#define HX711_DATA_BITS 24

/*
 * Figure 2's timing table. T3 is the PD_SCK high time: min 0.2 us, typ 1 us,
 * max 50 us. T4 is the low time: min 0.2 us, typ 1 us, and *no* maximum -- a
 * clock stretched on its low phase is harmless, which is why the driver
 * measures high phases individually rather than the burst as a whole.
 *
 * "Reset and Power-Down": PD_SCK high for longer than 60 us powers the device
 * down, and returning it low resets the device -- reverting channel and gain to
 * A/128.
 *
 * Note the gap. Anything over T3's 50 us is already outside the specified
 * timing, while only 60 us is documented to power down; between the two the
 * data sheet says nothing at all. A pulse in that window is therefore not
 * "probably fine", it is undefined, so the driver treats T3_MAX as the limit
 * and POWERDOWN_US only as the point past which the device state is known to
 * have been lost as well as the reading.
 *
 * Both are public because a caller reporting a timing violation has to quote
 * them: they are datasheet facts the explanation rests on, not tuning knobs.
 */
#define HX711_T3_MAX_US    50
#define HX711_POWERDOWN_US 60

/*
 * Conversions discarded after a channel or gain change: one possibly-stale,
 * plus the four output periods Table 2 requires for the analog path to settle.
 *
 * Public because a caller wants to say how long it is about to wait *before* it
 * waits, and the driver cannot say so without formatting text. The count is
 * fixed, so an announcement made from these agrees with what actually happens.
 */
#define HX711_SETTLING_CONVERSIONS 4
#define HX711_CHANGE_DISCARDS      (1 + HX711_SETTLING_CONVERSIONS)

/*
 * Error codes. The base is outside the range ESP-IDF assigns to itself, and
 * distinct from the other drivers in this repository so that esp_err_to_name()
 * cannot attribute one part's failure to another.
 */
#define ESP_ERR_HX711_BASE 0x33000
/*
 * A PD_SCK high phase exceeded T3. The reading was discarded -- past 60 us the
 * device also powers down mid-read and comes back at channel A gain 128, and
 * neither shows up in the number.
 */
#define ESP_ERR_HX711_CLOCK_STRETCHED (ESP_ERR_HX711_BASE + 1)
/*
 * DOUT did not return high after the burst. The 25th pulse is what pulls it
 * back high, so this is a positive test that the clock physically reached the
 * device -- the half of the wiring that reading data cannot prove.
 */
#define ESP_ERR_HX711_NO_CLOCK (ESP_ERR_HX711_BASE + 2)
/*
 * Every sample in a batch was identical. A 24-bit sigma-delta on a live bridge
 * always dithers, so this is a DOUT pin not carrying data -- and all-zeros in
 * particular reads as a perfectly plausible empty scale.
 */
#define ESP_ERR_HX711_STUCK_READING (ESP_ERR_HX711_BASE + 3)
/* A calibration move that the noise in the two averages could have produced. */
#define ESP_ERR_HX711_WITHIN_NOISE (ESP_ERR_HX711_BASE + 4)
/* A single conversion has no spread, so no honest uncertainty can be formed. */
#define ESP_ERR_HX711_TOO_FEW_SAMPLES (ESP_ERR_HX711_BASE + 5)

/*
 * Table 3, Input Channel and Gain Selection. These three are the whole of it:
 * channel B's gain is fixed at 32 and channel A cannot be set to it, so gain
 * and channel are not two settings but two views of one.
 *
 * Figure 2 annotates its third waveform "CH.B Gain:64", which is a typo -- both
 * Table 3 and the "Analog Inputs" prose on page 4 say 27 pulses select channel
 * A. Table 3 is the one to trust.
 *
 * The enumerator order is pulse-ascending (25, 26, 27), which is the order a
 * caller listing the settings will want.
 */
typedef enum {
    HX711_MODE_A128 = 0,  /* 25 pulses */
    HX711_MODE_B32  = 1,  /* 26 pulses */
    HX711_MODE_A64  = 2,  /* 27 pulses */
} hx711_mode_t;

typedef enum {
    HX711_CHANNEL_A = 0,
    HX711_CHANNEL_B = 1,
} hx711_channel_t;

/*
 * Encoding tables, exported so a caller can render the legal settings without
 * keeping a second copy of them.
 */
int hx711_mode_pulses(hx711_mode_t mode);  /* 25/26/27, -1 if invalid */
int hx711_mode_gain(hx711_mode_t mode);    /* 32/64/128, -1 if invalid */
hx711_channel_t hx711_mode_channel(hx711_mode_t mode);
esp_err_t hx711_mode_from_gain(int gain, hx711_mode_t *out);
extern const hx711_mode_t hx711_modes[3];

/*
 * The output rate is a strap on pin 15 -- low is 10 SPS, high is 80 SPS -- so
 * there is nothing to set and the only useful thing firmware can do is find out
 * which. Table 2 gives 10 and 80 as bare typical values, with no tolerance
 * column filled in at all, so real parts land wherever they land; the bands
 * behind hx711_rate_classify() are deliberately wide, because the two rates are
 * 8x apart and there is nothing to gain from tight ones.
 */
typedef enum {
    HX711_RATE_UNKNOWN = 0,  /* neither band: see the README */
    HX711_RATE_10SPS,
    HX711_RATE_80SPS,
} hx711_rate_t;

hx711_rate_t hx711_rate_classify(double sps);
/* Table 2 input noise at gain 128, in nV rms. -1 for HX711_RATE_UNKNOWN. */
int hx711_rate_input_noise_nv(hx711_rate_t rate);

/* Opaque handle. One HX711, on one pair of pins. */
typedef struct hx711_dev_t *hx711_handle_t;

/*
 * `dout_gpio` is the pin on the part's DOUT (an input here, pulled up) and
 * `sck_gpio` the one on PD_SCK (driven, parked low).
 *
 * hx711_create() records these and touches no hardware; the pins are validated
 * and claimed by hx711_bring_up(), so the two configuration failures can be
 * told apart by stage rather than collapsing into one error from a constructor.
 */
typedef struct {
    int dout_gpio;
    int sck_gpio;
} hx711_config_t;

/* Which step of the bring-up failed. */
typedef enum {
    HX711_STAGE_NONE = 0,       /* success */
    HX711_STAGE_PINS,           /* a pin does not exist, cannot drive, or they collide */
    HX711_STAGE_CONFIGURE_SCK,
    HX711_STAGE_CONFIGURE_DOUT,
    HX711_STAGE_FIRST_READY,    /* DOUT never went low after the power cycle */
    HX711_STAGE_SET_MODE,       /* the settling discards after the new pulse count */
    HX711_STAGE_MEASURE_RATE,
    HX711_STAGE_FIRST_READING,  /* the proof batch */
    HX711_STAGE_SET_SCALE,
    HX711_STAGE_SETTLE,         /* the discards inside set_mode/set_channel/power_up */
} hx711_stage_t;

/*
 * Statistics over a batch of conversions.
 *
 * `samples` is filled on failure too, as the number that completed, so a caller
 * can say which conversion of how many went wrong.
 *
 * `saturated` is advisory -- the driver still returns the batch. `all_identical`
 * is not: it comes back as ESP_ERR_HX711_STUCK_READING, because an exactly
 * repeating value is a wiring diagnosis rather than a data-quality note. That
 * is a deliberate divergence from components/nau7802, which has no equivalent
 * check.
 */
typedef struct {
    double mean;
    double stderr_mean;  /* standard error of the mean; 0 when samples < 2 */
    int32_t min;
    int32_t max;
    int samples;
    bool saturated;
    bool all_identical;
} hx711_stats_t;

/*
 * Host-side scale state.
 *
 * Mirrors components/nau7802's nau7802_scale_t field for field, because the
 * arithmetic above the converter is the same on both parts and two drivers that
 * disagreed about the shape of it would be two things to learn. The one
 * difference is that `tare_counts` is an integer here: this driver keeps the
 * tare as the conversion it actually came from.
 */
typedef struct {
    int32_t tare_counts;
    double tare_stderr;  /* standard error of the tare average */
    int tare_samples;    /* 0 until a tare has been taken */
    double counts_per_unit;
    bool calibrated;
    /*
     * The factor came from hx711_set_scale() rather than from a measurement
     * this session made. Worth carrying because it changes what the numbers
     * around it mean: `tare_stderr` still describes a live measurement, but
     * nothing here describes the accuracy of the factor itself, so a caller
     * quoting an error bar must not imply that it does.
     */
    bool supplied;
} hx711_scale_t;

/*
 * What a change to the analog path cost.
 *
 * Selecting a channel or gain means clocking a different number of trailing
 * pulses and then discarding conversions until the path has settled. The same
 * report covers hx711_set_mode(), hx711_set_channel() and hx711_power_up(),
 * because all three go through the one function that writes the mode.
 *
 * `scale_invalidated` is false on the power-up path without needing a special
 * case: coming back up re-applies the mode the driver already holds, so nothing
 * changed, so the tare and factor -- captured at that same gain -- still apply.
 * `scale_was_supplied` is captured *before* the reset destroys it, because
 * "calibrate again" is the wrong advice for a factor that was never measured
 * here and the provenance cannot be recovered afterwards.
 */
typedef struct {
    hx711_stage_t failed_stage;
    hx711_mode_t mode;  /* the setting now in force */
    bool mode_changed;
    int discards;
    int settling_conversions;
    bool scale_invalidated;
    bool scale_was_supplied;
} hx711_change_report_t;

/*
 * `mode` defaults to HX711_MODE_A128, which is both the part's reset default
 * and what a load cell normally wants -- so a zeroed struct is already right.
 *
 * `set_scale` installs a factor measured elsewhere -- on a bench, once, against
 * a known mass -- so a product does not have to re-derive it on every boot. It
 * is applied only if the whole bring-up succeeds, and only at the end of it,
 * which is also what makes it survive: bring-up power-cycles the part and drops
 * the host-side tare and scale, so a factor installed with hx711_set_scale()
 * *before* this call would be silently wiped.
 *
 * The tare is deliberately not part of this. It is the bridge's own zero, it
 * moves with temperature and mounting, and no constant compiled into a binary
 * can stand in for it. The flow is bring up with a factor, then hx711_tare(),
 * then hx711_weigh().
 */
typedef struct {
    hx711_mode_t mode;
    bool set_scale;
    double counts_per_unit;
} hx711_bringup_opts_t;

typedef struct {
    hx711_stage_t failed_stage;
    int dout_gpio;
    int sck_gpio;
    hx711_mode_t mode;  /* as applied */
    int discards;
    int settling_conversions;
    bool rate_measured;
    double sps;
    hx711_stats_t first_reading;  /* the proof batch */
    bool scale_supplied;          /* opts->set_scale was honoured */
} hx711_bringup_report_t;

typedef struct {
    double net_counts;
    double uncertainty;        /* counts, both averages combined in quadrature */
    double counts_per_unit;
    double precision_percent;
    hx711_mode_t mode;         /* the setting the measurement was taken at */
} hx711_calibration_t;

typedef struct {
    double units;
    double uncertainty_units;  /* 0 when samples < 2 */
    double spread_units;
    double net_counts;
    bool tare_taken;
} hx711_weight_t;

/*
 * A snapshot of driver state and two pad reads.
 *
 * Unlike components/nau7802's status, *nothing here is read back from silicon*
 * -- there is no bus and no register to read one from. Every field is either a
 * fact about the part that is true regardless, or something this driver set
 * itself, and a caller presenting the second as the first would be lying.
 *
 * This call measures nothing and always succeeds on a valid handle. To learn
 * the output rate, call hx711_measure_rate() explicitly: it costs conversions
 * and it can fail, and folding that into a status read would mean returning an
 * error with a partly valid out-param.
 */
typedef struct {
    bool ready;         /* hx711_bring_up() has succeeded */
    int dout_gpio;
    int sck_gpio;
    int dout_level;     /* -1 when powered down: the data sheet says nothing about it then */
    int sck_level;
    bool powered_down;
    hx711_mode_t mode;
    bool rate_measured;
    double sps;
    uint32_t worst_high_us;    /* worst PD_SCK high phase since bring-up */
    uint32_t stretched_bursts; /* how many broke T3 and were discarded */
    hx711_scale_t scale;
} hx711_status_t;

/* Records the pins and nothing else. Touches no hardware. */
esp_err_t hx711_create(const hx711_config_t *config, hx711_handle_t *out);

/*
 * Park PD_SCK low, release both pins, and free the handle.
 *
 * PD_SCK is deliberately released pulled *down* rather than in its reset state;
 * see the README. There is no hx711_set_pins(): a new pin pair is a new
 * instance, so delete and create again.
 */
esp_err_t hx711_delete(hx711_handle_t handle);

/*
 * Claim the pins, power-cycle the part, apply the mode, measure the output rate
 * and take a proof batch. `opts` may be NULL for the defaults.
 *
 * On failure the handle is left not-ready with the pins still claimed, so the
 * caller releases them by deleting it -- keeping claim and release symmetric.
 */
esp_err_t hx711_bring_up(hx711_handle_t handle, const hx711_bringup_opts_t *opts,
                         hx711_bringup_report_t *report);

bool hx711_is_ready(hx711_handle_t handle);
bool hx711_is_powered_down(hx711_handle_t handle);

/* Take one conversion. */
esp_err_t hx711_read_raw(hx711_handle_t handle, int32_t *out);

/*
 * Average `samples` conversions, tracking the spread.
 *
 * Welford, rather than accumulating a sum of squares: the mean here is a large
 * offset (a bridge sits tens of thousands of counts from zero) and subtracting
 * two big nearly-equal numbers at the end loses exactly the small variance
 * being measured.
 */
esp_err_t hx711_read_average(hx711_handle_t handle, int samples,
                             hx711_stats_t *stats);

/*
 * Time whole read cycles to establish the output rate. Costs conversions --
 * between four and sixteen, to a time budget -- and can fail.
 */
esp_err_t hx711_measure_rate(hx711_handle_t handle, double *sps);

esp_err_t hx711_get_mode(hx711_handle_t handle, hx711_mode_t *out);
esp_err_t hx711_set_mode(hx711_handle_t handle, hx711_mode_t mode,
                         hx711_change_report_t *report);
/*
 * Channel A has two gains, and naming the channel does not say which; this
 * returns to whichever was last in use, which is handle state.
 */
esp_err_t hx711_set_channel(hx711_handle_t handle, hx711_channel_t channel,
                            hx711_change_report_t *report);

/* Hold PD_SCK high. The part powers down after HX711_POWERDOWN_US. */
esp_err_t hx711_power_down(hx711_handle_t handle);
/* Release PD_SCK, wait for the part, and re-apply the configured mode. */
esp_err_t hx711_power_up(hx711_handle_t handle, hx711_change_report_t *report);

esp_err_t hx711_get_status(hx711_handle_t handle, hx711_status_t *out);
/* Worst PD_SCK high phase since bring-up. Monotonic, so reading it back after
 * ESP_ERR_HX711_CLOCK_STRETCHED gives the phase that failed. */
uint32_t hx711_worst_high_us(hx711_handle_t handle);

/*
 * Take the zero. Requires at least two samples for the uncertainty that
 * hx711_calibrate() later needs.
 */
esp_err_t hx711_tare(hx711_handle_t handle, int samples, hx711_stats_t *stats);

/*
 * Measure counts per unit against a known mass.
 *
 * Returns ESP_ERR_HX711_TOO_FEW_SAMPLES when either this batch or the tare has
 * fewer than two samples -- a single conversion has no spread, so the guard
 * below would compare against zero and accept anything -- and
 * ESP_ERR_HX711_WITHIN_NOISE when the move from the tare is smaller than ten
 * times the combined uncertainty of the two averages.
 *
 * The guard is on the uncertainty of the two *means*, not the spread of the
 * samples: peak-to-peak spread grows with the sample count while the
 * uncertainty of a mean falls as 1/sqrt(n), so a guard written against the
 * spread gets harder to satisfy the more you average -- making "take more
 * samples" the wrong advice and calibration unreachable at any sample count.
 *
 * `stats` and `result` are filled as far as the call got, so a caller can
 * report the numbers behind a refusal.
 */
esp_err_t hx711_calibrate(hx711_handle_t handle, double known_mass, int samples,
                          hx711_stats_t *stats, hx711_calibration_t *result);

esp_err_t hx711_weigh(hx711_handle_t handle, int samples, hx711_stats_t *stats,
                      hx711_weight_t *out);

/* Convert an existing batch to units, without taking another one. */
esp_err_t hx711_stats_to_weight(hx711_handle_t handle, const hx711_stats_t *in,
                                hx711_weight_t *out);

/*
 * Install a scale factor measured elsewhere, without measuring one here.
 *
 * Requires the part to be brought up, and that ordering is not negotiable:
 * hx711_bring_up() power-cycles the part and clears the tare and scale on the
 * way through, so a factor set first is silently gone by the time anything
 * could use it. Pass it in hx711_bringup_opts_t instead, which applies it once
 * the pins, the mode and the first real reading have all been proved.
 *
 * Sets the factor and nothing else. The tare stays a runtime measurement,
 * because it is the bridge's own zero and moves with temperature and mounting,
 * while a factor is a property of the cell and the gain, which do not.
 *
 * The factor is gain-specific, so hx711_set_mode() and hx711_set_channel() drop
 * it exactly as they drop a measured one. Power-down and power-up do not,
 * because the gain comes back unchanged.
 *
 * Rejects zero and non-finite values with ESP_ERR_INVALID_ARG -- both would
 * make every subsequent weight infinite or NaN, silently. A negative factor is
 * legitimate: a cell wired the other way round calibrates to one.
 */
esp_err_t hx711_set_scale(hx711_handle_t handle, double counts_per_unit);

/* The current tare and scale. Never NULL for a valid handle. */
const hx711_scale_t *hx711_get_scale(hx711_handle_t handle);

void hx711_reset_scale(hx711_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif
