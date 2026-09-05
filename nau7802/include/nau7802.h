/*
 * Nuvoton NAU7802 24-bit bridge ADC.
 *
 * A register-level driver for the part, plus the scale arithmetic (tare,
 * calibrate, weigh) that a load cell front end needs on top of it.
 *
 * Register addresses, bit positions and encodings come from the NAU7802 data
 * sheet, revision 1.5. Several of the choices here are not obvious from that
 * document and were established by measurement; each is explained where it
 * happens in the source, and summarised in README.md.
 *
 * This driver formats no text. Calls return esp_err_t and fill out-structs with
 * facts -- which stage of the power-up failed, whether the converter is
 * saturated and by how much, whether a calibration move was inside the noise --
 * so the caller can report them in whatever way suits it.
 */
#ifndef NAU7802_H
#define NAU7802_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed in silicon: the part has no address pins, so one per bus. */
#define NAU7802_I2C_ADDRESS 0x2A

/*
 * The result is 24-bit two's complement. Section 9.2 gives full scale as
 * +/-0.5 x (REFP - REFN) / gain, so the range is bipolar and this is 2^23.
 */
#define NAU7802_FULL_SCALE 8388608.0

/* Error codes. The base is outside the range ESP-IDF assigns to itself. */
#define ESP_ERR_NAU7802_BASE 0x30000
/* A calibration move that the noise in the two averages could have produced. */
#define ESP_ERR_NAU7802_WITHIN_NOISE (ESP_ERR_NAU7802_BASE + 1)
/* The device's own internal calibration reported CTRL2.CAL_ERR. */
#define ESP_ERR_NAU7802_CAL_FAILED (ESP_ERR_NAU7802_BASE + 2)
/* A single conversion has no spread, so no honest uncertainty can be formed. */
#define ESP_ERR_NAU7802_TOO_FEW_SAMPLES (ESP_ERR_NAU7802_BASE + 3)

/* GAINS[2:0]. The enumerator value is the register encoding. */
typedef enum {
    NAU7802_GAIN_1 = 0,
    NAU7802_GAIN_2 = 1,
    NAU7802_GAIN_4 = 2,
    NAU7802_GAIN_8 = 3,
    NAU7802_GAIN_16 = 4,
    NAU7802_GAIN_32 = 5,
    NAU7802_GAIN_64 = 6,
    NAU7802_GAIN_128 = 7,
} nau7802_gain_t;

/* CRS[2:0]. Encodings 4-6 are not defined by the data sheet. */
typedef enum {
    NAU7802_RATE_10SPS = 0,
    NAU7802_RATE_20SPS = 1,
    NAU7802_RATE_40SPS = 2,
    NAU7802_RATE_80SPS = 3,
    NAU7802_RATE_320SPS = 7,
} nau7802_rate_t;

/* VLDO[2:0]. Note that 000 is the *top* of the range, not the bottom. */
typedef enum {
    NAU7802_LDO_4V5 = 0,
    NAU7802_LDO_4V2 = 1,
    NAU7802_LDO_3V9 = 2,
    NAU7802_LDO_3V6 = 3,
    NAU7802_LDO_3V3 = 4,
    NAU7802_LDO_3V0 = 5,
    NAU7802_LDO_2V7 = 6,
    NAU7802_LDO_2V4 = 7,
} nau7802_ldo_t;

/* CTRL2.CHS. Channel A is the device's channel 1. */
typedef enum {
    NAU7802_CHANNEL_A = 0,
    NAU7802_CHANNEL_B = 1,
} nau7802_channel_t;

/*
 * Encoding tables.
 *
 * Exported so a caller can render the legal set, or map a number a user typed
 * onto an encoding, without keeping a second copy that can drift.
 */
int nau7802_gain_value(nau7802_gain_t gain);       /* 1..128, -1 if invalid */
int nau7802_rate_sps(nau7802_rate_t rate);         /* 10..320, -1 if undefined */
int nau7802_ldo_millivolts(nau7802_ldo_t ldo);     /* 2400..4500, -1 if invalid */

esp_err_t nau7802_gain_from_value(int value, nau7802_gain_t *out);
esp_err_t nau7802_rate_from_sps(int sps, nau7802_rate_t *out);
esp_err_t nau7802_ldo_from_millivolts(int millivolts, nau7802_ldo_t *out);

/* Every defined encoding, in ascending order, for printing "must be one of". */
extern const nau7802_gain_t nau7802_gains[8];
extern const nau7802_rate_t nau7802_rates[5];
extern const nau7802_ldo_t nau7802_ldos[8];

typedef struct nau7802_dev_t *nau7802_handle_t;

/*
 * How the part is wired.
 *
 * `dev` is owned by the caller and is not deleted by nau7802_delete(). Callers
 * that recreate their bus or their device handle must call
 * nau7802_set_device() rather than assuming the driver's copy is still live.
 * It may be NULL at creation, for an application that wants a handle before it
 * has a bus; every call that would talk to the device then returns
 * ESP_ERR_INVALID_STATE until nau7802_set_device() supplies one.
 *
 * `drdy_gpio` is the GPIO the device's DRDY output is wired to, or -1 to detect
 * conversions by polling PU_CTRL.CR over I2C instead. See nau7802_set_drdy()
 * for why the pin is worth wiring.
 */
typedef struct {
    i2c_master_dev_handle_t dev;
    int drdy_gpio;
} nau7802_config_t;

/*
 * What the power-up sequence should establish.
 *
 * `use_internal_ldo` false leaves AVDD taken from the pin, which is the chip
 * default: switching the internal regulator on while a board already drives
 * AVDD would put two sources on one net. Many load cell breakouts do need the
 * internal regulator.
 *
 * `set_gain` false leaves the gain at the power-up default of x1. Note that
 * bringing the part up always resets the registers, so a gain established by an
 * earlier nau7802_set_gain() does not survive.
 *
 * `set_scale` installs a scale factor measured elsewhere -- on a bench, once,
 * against a known mass -- so a product does not have to re-derive it on every
 * boot. It is applied only if the whole power-up succeeds, and only at the end
 * of it, which is also what makes it survive: bring-up drops the host-side tare
 * and scale exactly as a gain change does, so a factor installed with
 * nau7802_set_scale() *before* this call would be silently wiped. Belonging to
 * the same struct as `gain` is not cosmetic either -- a factor is counts per
 * unit at one gain, and the two are only ever correct together.
 *
 * The tare is deliberately not part of this. It is the bridge's own zero, it
 * moves with temperature, mounting and whatever is bolted to the cell, and no
 * constant compiled into a binary can stand in for it. The flow is bring up
 * with a factor, then nau7802_tare(), then nau7802_weigh().
 */
typedef struct {
    bool use_internal_ldo;
    nau7802_ldo_t ldo;
    bool set_gain;
    nau7802_gain_t gain;
    bool set_scale;
    double counts_per_unit;
} nau7802_bringup_opts_t;

/* Which step of the power-up sequence failed. */
typedef enum {
    NAU7802_STAGE_NONE = 0,      /* success */
    NAU7802_STAGE_PROBE,         /* reading DEVICE_REV */
    NAU7802_STAGE_RESET,
    NAU7802_STAGE_POWER_DIGITAL,
    NAU7802_STAGE_POWER_READY,   /* PU_CTRL.PUR never came up */
    NAU7802_STAGE_SET_LDO,
    NAU7802_STAGE_SET_GAIN,
    NAU7802_STAGE_POWER_ANALOG,
    NAU7802_STAGE_ADC_CTRL,      /* REG0x15 = 0x30 */
    NAU7802_STAGE_CALIBRATE,
    NAU7802_STAGE_START,         /* PU_CTRL.CS */
    NAU7802_STAGE_SETTLE,        /* the post-start discards */
} nau7802_stage_t;

/*
 * What the power-up actually established, read back from the device rather
 * than assumed. `gain` and `chopper_off` in particular are read back because
 * getting either wrong is invisible in the readings: a gain left at x1 puts a
 * load cell among the noise, and a chopper left at its power-up encoding costs
 * about six effective bits. Both still produce plausible-looking numbers.
 */
typedef struct {
    nau7802_stage_t failed_stage; /* NAU7802_STAGE_NONE on success */
    uint8_t revision;             /* DEVICE_REV, valid once PROBE has passed */
    nau7802_gain_t gain;          /* read back from CTRL1 */
    bool gain_was_requested;      /* false means this is the reset default */
    uint8_t adc_ctrl;             /* REG0x15 as read back */
    uint8_t chps;                 /* REG_CHPS[5:4]; 3 is the only valid value */
    bool chopper_off;             /* chps == 3 */
    bool adc_ctrl_valid;          /* the read-back itself succeeded */
    bool ldo_enabled;
    nau7802_ldo_t ldo;
    int drdy_gpio;                /* -1 when polling CR over I2C */
    int settling_discards;
} nau7802_bringup_report_t;

/*
 * The aftermath of a change to the analog path.
 *
 * Every setter that perturbs the analog path recalibrates the device, restarts
 * conversions, flushes the stale and unsettled ones, and drops the host-side
 * tare and scale. This reports what that cost, so a caller can say so.
 */
typedef struct {
    /*
     * NAU7802_STAGE_NONE on success. A failure here is one of CALIBRATE, START
     * or SETTLE -- worth distinguishing, because a timeout in the first is the
     * device not finishing its calibration and a timeout in the last is very
     * likely a DRDY pin that is not wired where it was said to be.
     */
    nau7802_stage_t failed_stage;
    bool scale_invalidated;     /* a tare or scale existed and has been dropped */
    /*
     * True when the scale just dropped was one the caller supplied rather than
     * one nau7802_calibrate() measured. The distinction cannot be recovered
     * afterwards -- the reset has already happened -- and it changes the advice
     * completely: "run calibrate again" is right for a measured factor and
     * wrong for a compiled-in one, where the fix is a factor measured at the
     * gain now in force.
     */
    bool scale_was_supplied;
    bool conversions_restarted; /* PU_CTRL.CS had been left clear */
    int discards;               /* conversions thrown away */
    int settling_conversions;   /* how many of those were filter settling */
} nau7802_change_report_t;

/* The result of a batch of conversions. */
typedef struct {
    double mean;
    /*
     * Standard error of the mean. Zero when samples < 2 -- which is not a claim
     * of perfect knowledge. A caller using this number must check `samples` and
     * refuse, because zero uncertainty accepts anything.
     */
    double stderr_mean;
    int32_t min;
    int32_t max;
    int samples;
    /*
     * True when either extreme reached 99% of full scale. Advisory: the data is
     * still returned. A converter pinned at a rail usually means the bridge is
     * disconnected or unexcited, or the gain is too high for the signal -- all
     * of which otherwise look like a large, confident number.
     */
    bool saturated;
    double saturation_percent; /* the worse extreme, as a percentage */
} nau7802_stats_t;

/* Host-side scale state. Read-only; see nau7802_tare()/nau7802_calibrate(). */
typedef struct {
    double tare_counts;
    double tare_stderr;  /* standard error of the tare average */
    int tare_samples;    /* 0 until a tare has been taken */
    double counts_per_unit;
    bool calibrated;
    /*
     * The factor came from nau7802_set_scale() rather than from a measurement
     * this session made. Worth carrying because it changes what the numbers
     * around it mean: `tare_stderr` still describes a live measurement, but
     * nothing here describes the accuracy of the factor itself, so a caller
     * quoting an error bar must not imply that it does.
     */
    bool supplied;
} nau7802_scale_t;

/* The outcome of nau7802_calibrate(), valid whether it succeeded or refused. */
typedef struct {
    double net_counts;        /* the move away from the tare */
    double uncertainty;       /* the two averages' standard errors in quadrature */
    double counts_per_unit;   /* valid on success */
    double precision_percent; /* valid on success */
    nau7802_gain_t gain;      /* read back, so a refusal can name a low gain */
    bool gain_valid;
} nau7802_calibration_t;

/* The outcome of nau7802_weigh(). */
typedef struct {
    double units;
    double uncertainty_units; /* 0 when samples < 2; quote no error bar then */
    double spread_units;      /* peak-to-peak, i.e. what one reading looks like */
    double net_counts;
    /*
     * False when no tare has been taken, in which case `units` is the bridge's
     * own offset reported as load -- tens of thousands of counts on an unloaded
     * cell. Only reachable with a supplied factor: nau7802_calibrate() refuses
     * without a tare of at least two samples, so until nau7802_set_scale()
     * existed, `calibrated` implied a real zero. It no longer does.
     */
    bool tare_taken;
} nau7802_weight_t;

/* Everything nau7802_get_status() can establish about a device, brought up by
 * this driver or not. */
typedef struct {
    uint8_t revision;
    uint8_t pu_ctrl;
    uint8_t ctrl1;
    uint8_t ctrl2;
    bool digital_up;
    bool analog_up;
    bool power_ready;
    bool data_ready;
    bool conversions_started;
    bool internal_ldo;
    nau7802_gain_t gain;
    nau7802_ldo_t ldo;
    nau7802_rate_t rate;
    int rate_sps; /* -1 when the register holds an undefined encoding */
    nau7802_channel_t channel;
    bool cal_error;

    /* Read separately; false if that read failed. */
    bool pga_valid;
    uint8_t pga;
    uint8_t power;
    bool ldomode;
    bool pga_cap;

    bool adc_ctrl_valid;
    uint8_t adc_ctrl;
    uint8_t chps;
    bool chopper_off;

    /* The calibration registers of the currently selected channel. */
    bool cal_regs_valid;
    uint32_t ocal_raw;
    int32_t ocal_counts; /* decoded; the encoding is sign-magnitude */
    uint32_t gcal_raw;
    double gcal_ratio; /* 1.0 exactly when GCAL reads 0x00800000 */

    int drdy_gpio;   /* -1 when polling CR over I2C */
    int drdy_level;  /* -1 when there is no pin */
    bool brought_up; /* nau7802_bring_up() has succeeded on this handle */
    nau7802_scale_t scale;
} nau7802_status_t;

/* One row of the device's register map, for a diagnostic dump. */
typedef struct {
    uint8_t reg;
    const char *name;
} nau7802_register_info_t;

/*
 * Create a driver instance. Allocates and claims the DRDY pin if one is given,
 * but issues no I2C traffic -- so a handle is usable for nau7802_get_status()
 * and nau7802_read_register() against a device this session did not bring up,
 * and can be created before a bus exists at all.
 */
esp_err_t nau7802_create(const nau7802_config_t *config, nau7802_handle_t *out);

/* Release the DRDY pin and free the handle. The I2C device handle is the
 * caller's and is left alone. */
esp_err_t nau7802_delete(nau7802_handle_t handle);

/* Point the driver at a different (or recreated) I2C device handle. Issues no
 * traffic and does not disturb the device. */
esp_err_t nau7802_set_device(nau7802_handle_t handle, i2c_master_dev_handle_t dev);

/*
 * Reset, configure and power up the converter, then run its internal offset
 * calibration and leave conversions running.
 *
 * The ordering inside this is load-bearing and is not the obvious one; see the
 * comments in nau7802.c. Re-runnable. On failure `report->failed_stage` names
 * the step, and the driver is left not brought up.
 *
 * `report` may be NULL.
 */
esp_err_t nau7802_bring_up(nau7802_handle_t handle,
                           const nau7802_bringup_opts_t *opts,
                           nau7802_bringup_report_t *report);

/* True once nau7802_bring_up() has succeeded on this handle. */
bool nau7802_is_ready(nau7802_handle_t handle);

/*
 * Wire (or release, with gpio < 0) the DRDY line.
 *
 * The device raises DRDY when a conversion lands and drops it when the result
 * registers are read. That is the same information as PU_CTRL.CR, but without
 * an I2C transaction and without polling latency -- and the latency is the
 * point. Polling CR runs on the FreeRTOS tick, so the burst read of the result
 * can start at an unknown phase within the conversion, sometimes exactly as the
 * device rewrites the registers. The result is then stitched from two
 * conversions: near zero that is a +/-65,500 outlier on an input that is not
 * moving, and anywhere else it passes for noise.
 *
 * Takes effect immediately and independently of bring-up, because which pin
 * DRDY lands on is a statement about the board rather than about the converter.
 */
esp_err_t nau7802_set_drdy(nau7802_handle_t handle, int gpio);
int nau7802_drdy_gpio(nau7802_handle_t handle);  /* -1 when polling */
int nau7802_drdy_level(nau7802_handle_t handle); /* -1 when there is no pin */

/* Take one conversion, waiting for the device to signal that it is ready. */
esp_err_t nau7802_read_raw(nau7802_handle_t handle, int32_t *out);

/*
 * Average `samples` conversions.
 *
 * Load cell readings are noisy enough that a single sample is rarely
 * meaningful. The variance is accumulated with Welford's method rather than as
 * a sum of squares: the mean here is a large offset -- a bridge sits tens of
 * thousands of counts from zero -- and subtracting two big nearly-equal numbers
 * at the end loses exactly the small variance being measured.
 */
esp_err_t nau7802_read_average(nau7802_handle_t handle, int samples,
                               nau7802_stats_t *out);

/*
 * Configuration.
 *
 * Each setter below changes the analog path, so each one re-runs the device's
 * internal offset calibration, restarts conversions, discards the stale and
 * unsettled ones, and drops the host-side tare and scale. That sequencing is
 * device knowledge and is deliberately not left to the caller to rediscover:
 * skipping it leaves readings that look right and are not. `report` may be NULL.
 *
 * The getters read the register back rather than mirroring it in a variable.
 */
esp_err_t nau7802_get_gain(nau7802_handle_t handle, nau7802_gain_t *out);
esp_err_t nau7802_set_gain(nau7802_handle_t handle, nau7802_gain_t gain,
                           nau7802_change_report_t *report);

esp_err_t nau7802_get_rate(nau7802_handle_t handle, nau7802_rate_t *out);
esp_err_t nau7802_set_rate(nau7802_handle_t handle, nau7802_rate_t rate,
                           nau7802_change_report_t *report);

esp_err_t nau7802_get_channel(nau7802_handle_t handle, nau7802_channel_t *out);
esp_err_t nau7802_set_channel(nau7802_handle_t handle, nau7802_channel_t channel,
                              nau7802_change_report_t *report);

/*
 * REG0x1B[6] LDOMODE picks the compensation for the internal regulator's
 * control loop, and it has to match the capacitor the board fits on AVDD:
 * false (the chip default) expects an ESR below 1 ohm, true tolerates up to 5.
 * A board that fits something with more ESR than the default expects runs a
 * marginally compensated regulator, which is a broadband noise source on the
 * supply and the reference -- after the PGA, so no amount of gain or input
 * rewiring changes it.
 */
esp_err_t nau7802_get_ldomode(nau7802_handle_t handle, bool *out);
esp_err_t nau7802_set_ldomode(nau7802_handle_t handle, bool stable,
                              nau7802_change_report_t *report);

/*
 * REG0x1C[7] PGA_CAP_EN connects a filter capacitor across the VIN2P/VIN2N pins
 * to the PGA output, "for enhanced ENOB at high PGA gain settings". It needs
 * the capacitor to be physically fitted -- 330 pF at AVDD 3.3 V, 680 pF at
 * 4.5 V -- and it consumes channel B, whose pins become the filter node. With
 * no capacitor there it changes nothing, silently.
 */
esp_err_t nau7802_get_pga_cap(nau7802_handle_t handle, bool *out);
esp_err_t nau7802_set_pga_cap(nau7802_handle_t handle, bool enable,
                              nau7802_change_report_t *report);

/* Read everything the device will say about itself. Does not require bring-up. */
esp_err_t nau7802_get_status(nau7802_handle_t handle, nau7802_status_t *out);

/* Single register read, for a diagnostic dump. Does not require bring-up. */
esp_err_t nau7802_read_register(nau7802_handle_t handle, uint8_t reg, uint8_t *out);

/* The whole register map with names, for that dump. */
const nau7802_register_info_t *nau7802_register_map(size_t *count);

/*
 * Scale arithmetic.
 *
 * The device's internal calibration handles its own offset; these are the
 * host-side tare and scale factor that turn counts into whatever unit the
 * known mass was given in.
 */

/* Capture the zero offset. `stats` may be NULL. */
esp_err_t nau7802_tare(nau7802_handle_t handle, int samples, nau7802_stats_t *stats);

/*
 * Derive the scale from a known mass placed after nau7802_tare().
 *
 * Refuses with ESP_ERR_NAU7802_WITHIN_NOISE when the move away from the tare is
 * under ten times the combined standard error of the two averages, and with
 * ESP_ERR_NAU7802_TOO_FEW_SAMPLES when either batch was a single conversion.
 *
 * The guard is on the uncertainty of the two *averages*, not on the spread of
 * the samples, and the difference matters more than it looks: peak-to-peak
 * spread grows with the sample count while the uncertainty of a mean falls as
 * 1/sqrt(n). A guard written against the spread gets harder to satisfy the more
 * you average, so "take more samples" -- the one correct response to a noisy
 * part -- makes the refusal worse instead of better.
 *
 * `stats` and `result` may be NULL; `result` is filled on refusal too.
 */
esp_err_t nau7802_calibrate(nau7802_handle_t handle, double known_mass, int samples,
                            nau7802_stats_t *stats, nau7802_calibration_t *result);

/* Report the load in calibrated units. `stats` may be NULL. */
esp_err_t nau7802_weigh(nau7802_handle_t handle, int samples,
                        nau7802_stats_t *stats, nau7802_weight_t *out);

/*
 * Install a scale factor measured elsewhere, without measuring one here.
 *
 * A factory calibration is a bench measurement made once against a known mass;
 * this is how the resulting constant gets back into a device that has no reason
 * to re-derive it on every boot. nau7802_calibrate() remains the way to produce
 * that constant -- run it on the bench and keep what it reports.
 *
 * Sets the factor and nothing else. The tare stays a runtime measurement,
 * because it is the bridge's own zero and moves with temperature and mounting,
 * while a factor is a property of the cell and the gain, which do not. Until
 * nau7802_tare() has run, nau7802_weigh() reports `tare_taken` false and its
 * reading is the unloaded offset rather than a weight.
 *
 * The factor is gain- and channel-specific, so every setter that touches the
 * analog path drops it again, and so does bring-up. Passing it through
 * nau7802_bringup_opts_t instead ties it to the gain set in the same call, and
 * is the form to prefer.
 *
 * A negative factor is legitimate: a cell wired the other way round calibrates
 * to one, and everything downstream already handles the sign. Zero and
 * non-finite values are rejected with ESP_ERR_INVALID_ARG -- both would make
 * every subsequent weight infinite or NaN, silently.
 *
 * Returns ESP_ERR_INVALID_STATE if the part has not been brought up, since
 * bring-up would drop the factor again anyway.
 */
esp_err_t nau7802_set_scale(nau7802_handle_t handle, double counts_per_unit);

/* The current tare and scale. Never NULL for a valid handle. */
const nau7802_scale_t *nau7802_get_scale(nau7802_handle_t handle);

/* Drop the tare and scale, as a change to the analog path would. */
void nau7802_reset_scale(nau7802_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* NAU7802_H */
