/*
 * Avia Semiconductor HX711 24-bit ADC for weigh scales.
 *
 * See include/hx711.h for the API and README.md for the datasheet quirks that
 * are not obvious from the document itself.
 */
#include "hx711_priv.h"

#include <math.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"

static const struct {
    int pulses;
    hx711_channel_t channel;
    int gain;
} mode_table[] = {
    [HX711_MODE_A128] = {HX711_PULSES_A128, HX711_CHANNEL_A, 128},
    [HX711_MODE_B32]  = {HX711_PULSES_B32,  HX711_CHANNEL_B,  32},
    [HX711_MODE_A64]  = {HX711_PULSES_A64,  HX711_CHANNEL_A,  64},
};

const hx711_mode_t hx711_modes[3] = {
    HX711_MODE_A128, HX711_MODE_B32, HX711_MODE_A64,
};

static bool mode_valid(hx711_mode_t mode)
{
    return mode == HX711_MODE_A128 || mode == HX711_MODE_B32 ||
           mode == HX711_MODE_A64;
}

int hx711_mode_pulses(hx711_mode_t mode)
{
    return mode_valid(mode) ? mode_table[mode].pulses : -1;
}

int hx711_mode_gain(hx711_mode_t mode)
{
    return mode_valid(mode) ? mode_table[mode].gain : -1;
}

hx711_channel_t hx711_mode_channel(hx711_mode_t mode)
{
    return mode_valid(mode) ? mode_table[mode].channel : HX711_CHANNEL_A;
}

esp_err_t hx711_mode_from_gain(int gain, hx711_mode_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(hx711_modes) / sizeof(hx711_modes[0]); i++) {
        if (mode_table[hx711_modes[i]].gain == gain) {
            *out = hx711_modes[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

hx711_rate_t hx711_rate_classify(double sps)
{
    if (sps >= RATE_10SPS_LO && sps <= RATE_10SPS_HI) {
        return HX711_RATE_10SPS;
    }
    if (sps >= RATE_80SPS_LO && sps <= RATE_80SPS_HI) {
        return HX711_RATE_80SPS;
    }
    return HX711_RATE_UNKNOWN;
}

int hx711_rate_input_noise_nv(hx711_rate_t rate)
{
    switch (rate) {
    case HX711_RATE_10SPS: return 50;
    case HX711_RATE_80SPS: return 90;
    case HX711_RATE_UNKNOWN: break;
    }
    return -1;
}

/*
 * Bare-register pad access.
 *
 * These are the only pin access that can be inlined into an IRAM function.
 * gpio_set_level() is not an option here: it is only placed in IRAM when
 * CONFIG_GPIO_CTRL_FUNC_IN_IRAM is set, which would quietly defeat the
 * IRAM_ATTR on clock_burst().
 */
static inline void pad_write(int pin, int level)
{
#ifdef GPIO_OUT1_W1TS_REG
    if (pin >= 32) {
        REG_WRITE(level ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, BIT(pin - 32));
        return;
    }
#endif
    REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, BIT(pin));
}

static inline int pad_level(int pin)
{
#ifdef GPIO_IN1_REG
    if (pin >= 32) {
        return (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1;
    }
#endif
    return (REG_READ(GPIO_IN_REG) >> pin) & 1;
}

static uint32_t cycles_to_us(uint32_t cycles)
{
    const uint32_t hz = (uint32_t)esp_clk_cpu_freq();
    if (hz == 0) {
        return 0;
    }
    return (uint32_t)((uint64_t)cycles * 1000000ULL / hz);
}

/*
 * Wait for the device to pull DOUT low.
 *
 * `precise` busy-polls, which resolves the conversion boundary to about 20 us
 * but starves every lower-priority task for as long as it waits. Only
 * measure_rate() asks for it, and only inside its own budget -- a whole batch
 * taken that way would hold the idle task off for tens of seconds and trip the
 * task watchdog. Everything else blocks on the FreeRTOS tick.
 *
 * Tick latency is harmless here in a way it is not on the NAU7802, and the
 * difference is worth being explicit about because the two drivers otherwise
 * look alike. The NAU7802 rewrites its result registers at end-of-conversion
 * whatever the bus is doing, so a read that starts at an unknown phase can
 * straddle that moment and stitch two conversions together -- which is what its
 * DRDY line exists to prevent. The HX711 holds its result until the host clocks
 * it out, so a read that starts late is a late read of the right conversion.
 * There is nothing here for an interrupt-driven ready line to buy.
 */
static esp_err_t wait_ready(hx711_handle_t handle, uint32_t timeout_ms,
                            bool precise)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (pad_level(handle->dout_pin) != 0) {
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        if (precise) {
            esp_rom_delay_us(HX711_READY_POLL_US);
        } else {
            /* vTaskDelay(n) guarantees only (n-1) whole tick periods; the extra
             * tick is the same idiom the SHT4x driver (components/sht4x) and
             * the NAU7802 driver (components/nau7802) use. */
            vTaskDelay(pdMS_TO_TICKS(1) + 1);
        }
    }
    return ESP_OK;
}

/*
 * The spinlock is a file static rather than handle state, and the reason
 * differs from every other field here.
 *
 * It is not state at all -- its scope is "one bit-banged burst at a time in
 * this process", which is a property of the CPU rather than of any one device.
 * Per-handle locks would still mask interrupts correctly on a uniprocessor, but
 * on a dual-core target they would permit two bursts to run simultaneously on
 * two cores, contending for exactly the resources -- instruction cache, flash
 * access -- whose stalls the T3 check below exists to catch.
 */
static portMUX_TYPE hx711_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * Clock one conversion out, plus the trailing pulses that select the next one's
 * channel and gain.
 *
 * IRAM_ATTR is load-bearing, not decoration. The critical section masks
 * interrupts, so nothing can preempt this -- but code executing from flash can
 * still stall on an instruction-cache miss, and a flash write elsewhere in the
 * system disables the cache outright for milliseconds. Either would stretch
 * whichever PD_SCK high phase it landed in past 60 us and power the device down
 * mid-transfer, which does not produce a late reading: it produces a plausible
 * number from a device that reset and reverted to channel A gain 128.
 *
 * For that to hold, nothing in here may reach flash. No calls except
 * esp_rom_delay_us() (in ROM, always mapped) and the inlined pad accessors, and
 * *no .rodata* -- which is why the pulse count arrives as a parameter rather
 * than being looked up in mode_table[], and why this function reports nothing
 * and returns facts for the caller to interpret.
 */
static void IRAM_ATTR clock_burst(int dout, int sck, int pulses,
                                  uint32_t *value, uint32_t *worst_high_cycles,
                                  int *dout_after)
{
    uint32_t bits = 0;
    uint32_t worst = 0;

    /*
     * About 60 us of masked interrupts: 27 pulses of 1 us high and 1 us low,
     * plus two cycle-counter reads each. The burst cannot be split -- a gap
     * between pulses is exactly what the critical section is here to prevent.
     */
    portENTER_CRITICAL(&hx711_lock);
    for (int i = 0; i < pulses; i++) {
        pad_write(sck, 1);
        const uint32_t rise = esp_cpu_get_cycle_count();
        esp_rom_delay_us(HX711_SCK_HIGH_US);

        /* T2: the bit is valid within 0.1 us of the rising edge, so sampling at
         * the end of the high phase is inside the window the data sheet
         * actually describes. (Its own reference driver samples after the
         * falling edge instead, which also works, since the bit is held until
         * the next rise.) */
        const int level = pad_level(dout);
        const uint32_t held = esp_cpu_get_cycle_count() - rise;
        pad_write(sck, 0);

        if (held > worst) {
            worst = held;
        }
        if (i < HX711_DATA_BITS) {
            bits = (bits << 1) | (uint32_t)level;
        }

        esp_rom_delay_us(HX711_SCK_LOW_US);
    }
    const int after = pad_level(dout);
    portEXIT_CRITICAL(&hx711_lock);

    *value = bits;
    *worst_high_cycles = worst;
    *dout_after = after;
}

/* One conversion, with the timing accounting the diagnostics depend on. */
static esp_err_t read_one(hx711_handle_t handle, int32_t *out)
{
    esp_err_t err = wait_ready(handle, HX711_READY_TIMEOUT_MS, false);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t raw = 0;
    uint32_t worst_cycles = 0;
    int dout_after = 0;
    clock_burst(handle->dout_pin, handle->sck_pin,
                mode_table[handle->mode].pulses, &raw, &worst_cycles,
                &dout_after);

    const uint32_t worst_us = cycles_to_us(worst_cycles);
    if (worst_us > handle->worst_high_us) {
        handle->worst_high_us = worst_us;
    }

    /*
     * A stretched high phase invalidates the reading, and past 60 us it also
     * silently reset the device's channel and gain. Neither shows up in the
     * number: an unflagged one comes back at a believable magnitude, at a gain
     * nobody asked for, with the tare and scale still applied.
     */
    if (worst_us > HX711_T3_MAX_US) {
        handle->stretched_bursts++;
        return ESP_ERR_HX711_CLOCK_STRETCHED;
    }

    /*
     * "The 25th pulse at PD_SCK input will pull DOUT pin back to high." Nothing
     * else drives DOUT high inside a conversion period, so this is a positive
     * test that the clock physically reached the device -- the half of the
     * wiring that reading data cannot prove on its own.
     */
    if (dout_after == 0) {
        return ESP_ERR_HX711_NO_CLOCK;
    }

    /*
     * Table 2: two's complement, 0x800000 to 0x7FFFFF. Sign-extend into
     * int32_t, exactly as components/nau7802 does.
     *
     * The data sheet's own reference driver on page 8 ends with
     * `Count = Count ^ 0x800000`, which returns offset binary instead and
     * contradicts its own Table 2. Tare and calibrate would hide it -- the
     * offset cancels and the scale is linear -- but a raw reading would be
     * nonsense and the rail check would compare against the wrong numbers.
     * This is not that.
     */
    if (raw & 0x800000) {
        raw |= 0xFF000000;
    }
    *out = (int32_t)raw;
    return ESP_OK;
}

static esp_err_t require_ready(hx711_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->ready || handle->powered_down) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t hx711_read_raw(hx711_handle_t handle, int32_t *out)
{
    esp_err_t err = require_ready(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_one(handle, out);
}

static esp_err_t discard_conversions(hx711_handle_t handle, int count)
{
    for (int i = 0; i < count; i++) {
        int32_t ignored = 0;
        esp_err_t err = read_one(handle, &ignored);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t hx711_priv_read_average(hx711_handle_t handle, int samples,
                                  hx711_stats_t *stats)
{
    esp_err_t err = require_ready(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (!stats || samples < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    *stats = (hx711_stats_t){0};

    double total = 0.0;
    int32_t low = INT32_MAX;
    int32_t high = INT32_MIN;

    /* Welford, rather than accumulating a sum of squares: the mean here is a
     * large offset (a bridge sits tens of thousands of counts from zero) and
     * subtracting two big nearly-equal numbers at the end loses exactly the
     * small variance being measured. */
    double running_mean = 0.0;
    double m2 = 0.0;

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        err = read_one(handle, &value);
        if (err != ESP_OK) {
            /* `samples` is how many completed, so the caller can say which
             * conversion of how many went wrong. */
            stats->samples = i;
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

    stats->samples = samples;
    stats->mean = total / samples;
    stats->min = low;
    stats->max = high;

    /*
     * A single conversion has no spread, so there is nothing to estimate a
     * noise figure from. This reports 0, which is *not* a claim of perfect
     * knowledge -- callers that use the number must check the sample count and
     * refuse, because a zero uncertainty would let a calibration accept any
     * change at all, which is the exact failure its guard exists to prevent.
     */
    const double variance = samples > 1 ? m2 / (samples - 1) : 0.0;
    stats->stderr_mean = samples > 1 ? sqrt(variance / samples) : 0.0;

    /*
     * A converter pinned at either rail is not measuring anything. During
     * bringup that usually means the bridge is disconnected or miswired, the
     * excitation is missing, or the gain is too high for the signal. Advisory:
     * the batch is still returned.
     */
    stats->saturated = (low <= -(int32_t)(HX711_FULL_SCALE - 1) ||
                        high >= (int32_t)(HX711_FULL_SCALE - 1));

    /*
     * Every reading identical is not a quiet signal, it is no signal. Table 2
     * puts the input noise at 50 nV rms at gain 128, which is several counts,
     * so a live bridge always dithers. An exactly repeating value is what a
     * DOUT pin that is not carrying data looks like -- and all-zeros in
     * particular reads as a perfectly plausible empty scale.
     */
    if (samples >= 4 && low == high) {
        stats->all_identical = true;
        return ESP_ERR_HX711_STUCK_READING;
    }

    return ESP_OK;
}

esp_err_t hx711_read_average(hx711_handle_t handle, int samples,
                             hx711_stats_t *stats)
{
    return hx711_priv_read_average(handle, samples, stats);
}

/*
 * The only writer of `mode`.
 *
 * Selecting a channel or gain means clocking the right number of trailing
 * pulses and then throwing conversions away until the analog path has settled.
 * Every path that changes the setting goes through here, because a change that
 * skips the discard is invisible: the next reading comes back at the previous
 * gain, off by exactly a factor of two or four, and self-consistent.
 *
 * It also drops the tare and the scale, which were captured at the old gain and
 * are wrong by the same factor. Losing a calibration loudly beats keeping one
 * that quietly reports the wrong weight.
 */
static esp_err_t apply_mode(hx711_handle_t handle, hx711_mode_t requested,
                            hx711_change_report_t *report)
{
    const bool changed = (requested != handle->mode) || !handle->ready;

    handle->mode = requested;
    if (mode_table[handle->mode].channel == HX711_CHANNEL_A) {
        handle->last_a_mode = handle->mode;
    }

    const int discards = HX711_STALE_CONVERSIONS + HX711_SETTLING_CONVERSIONS;

    if (report) {
        report->failed_stage = HX711_STAGE_NONE;
        report->mode = handle->mode;
        report->mode_changed = changed;
        report->discards = discards;
        report->settling_conversions = HX711_SETTLING_CONVERSIONS;
        /*
         * Captured before the reset below destroys it: the provenance cannot be
         * recovered afterwards, and "calibrate again" is the wrong advice for a
         * factor that was never measured here.
         */
        report->scale_was_supplied = handle->scale.supplied;
        report->scale_invalidated = changed && handle->scale.calibrated;
    }

    /* The first burst carries the new pulse count and so selects the setting;
     * the rest are the settling time. */
    esp_err_t err = discard_conversions(handle, discards);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = HX711_STAGE_SETTLE;
        }
        return err;
    }

    if (changed) {
        handle->scale = (hx711_scale_t){0};
    }
    return ESP_OK;
}

esp_err_t hx711_measure_rate(hx711_handle_t handle, double *sps)
{
    esp_err_t err = require_ready(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (!sps) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * A conversion is discarded first so timing starts on a conversion
     * boundary; without that the first wait lands at an arbitrary phase and
     * costs up to a whole period of error. The loop then runs to a time budget
     * rather than a fixed count, which keeps 10 SPS to its four-conversion
     * floor and stops 80 SPS from taking a second to prove itself.
     */
    err = discard_conversions(handle, 1);
    if (err != ESP_OK) {
        return err;
    }

    const int64_t start = esp_timer_get_time();
    int count = 0;

    while (count < RATE_MAX_CONVERSIONS &&
           (count < RATE_MIN_CONVERSIONS ||
            esp_timer_get_time() - start < RATE_BUDGET_US)) {
        err = wait_ready(handle, HX711_READY_TIMEOUT_MS, true);
        if (err != ESP_OK) {
            return err;
        }

        uint32_t raw = 0, worst = 0;
        int after = 0;
        clock_burst(handle->dout_pin, handle->sck_pin,
                    mode_table[handle->mode].pulses, &raw, &worst, &after);
        count++;

        /* These conversions are thrown away, but the timing they were taken
         * with is not: the worst high phase is claimed to be the worst since
         * bring-up, so it has to include the bursts taken here. */
        const uint32_t worst_us = cycles_to_us(worst);
        if (worst_us > handle->worst_high_us) {
            handle->worst_high_us = worst_us;
        }
    }

    const int64_t elapsed = esp_timer_get_time() - start;
    if (elapsed <= 0 || count == 0) {
        return ESP_FAIL;
    }

    *sps = (double)count * 1000000.0 / (double)elapsed;
    handle->measured_sps = *sps;
    return ESP_OK;
}

esp_err_t hx711_create(const hx711_config_t *config, hx711_handle_t *out)
{
    if (!config || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    struct hx711_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    dev->dout_pin = config->dout_gpio;
    dev->sck_pin = config->sck_gpio;
    dev->mode = HX711_MODE_A128;
    dev->last_a_mode = HX711_MODE_A128;

    *out = dev;
    return ESP_OK;
}

static void release_pins(hx711_handle_t handle)
{
    if (handle->sck_pin >= 0) {
        gpio_reset_pin((gpio_num_t)handle->sck_pin);
        /*
         * gpio_reset_pin() enables the pull-up -- "for powersave reasons, the
         * GPIO should not be floating" -- which on this pin is not a cosmetic
         * default but a command: PD_SCK held high for 60 us powers the HX711
         * down. Handing the pin back in its reset state therefore powers the
         * part off behind the caller's back, and the next bring-up finds a
         * device that has silently reverted to channel A gain 128.
         *
         * So this one pin is deliberately released pulled *down* instead. It is
         * the only level at which the part keeps converting with nothing
         * driving it.
         */
        gpio_pullup_dis((gpio_num_t)handle->sck_pin);
        gpio_pulldown_en((gpio_num_t)handle->sck_pin);
    }
    if (handle->dout_pin >= 0) {
        gpio_reset_pin((gpio_num_t)handle->dout_pin);
    }
    handle->sck_pin = -1;
    handle->dout_pin = -1;
}

esp_err_t hx711_delete(hx711_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Leave the clock low on the way out; see release_pins() for why the pin
     * cannot simply be reset. */
    if (handle->ready && handle->sck_pin >= 0) {
        pad_write(handle->sck_pin, 0);
    }
    release_pins(handle);
    free(handle);
    return ESP_OK;
}

bool hx711_is_ready(hx711_handle_t handle)
{
    return handle && handle->ready;
}

bool hx711_is_powered_down(hx711_handle_t handle)
{
    return handle && handle->powered_down;
}

uint32_t hx711_worst_high_us(hx711_handle_t handle)
{
    return handle ? handle->worst_high_us : 0;
}

esp_err_t hx711_bring_up(hx711_handle_t handle, const hx711_bringup_opts_t *opts,
                         hx711_bringup_report_t *report)
{
    static const hx711_bringup_opts_t defaults = {0};
    if (!opts) {
        opts = &defaults;
    }
    if (report) {
        *report = (hx711_bringup_report_t){0};
    }
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    const int dout = handle->dout_pin;
    const int sck = handle->sck_pin;

    if (report) {
        report->dout_gpio = dout;
        report->sck_gpio = sck;
    }

    /*
     * What this checks is what the part and the SoC require: the pins exist,
     * PD_SCK can be an output, and they are not the same wire. Whether some
     * other subsystem is entitled to the pin is the caller's policy, not the
     * driver's, and a driver that refused on those grounds would be unusable in
     * a project that has no such policy.
     */
    if (!GPIO_IS_VALID_GPIO(dout) || !GPIO_IS_VALID_OUTPUT_GPIO(sck) ||
        dout == sck || !mode_valid(opts->mode)) {
        if (report) {
            report->failed_stage = HX711_STAGE_PINS;
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Park the clock low before the pad is an output, so configuring it cannot
     * glitch high and start a power-down. */
    pad_write(sck, 0);

    const gpio_config_t sck_config = {
        .pin_bit_mask = BIT64(sck),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&sck_config);
    if (err != ESP_OK) {
        if (report) {
            report->failed_stage = HX711_STAGE_CONFIGURE_SCK;
        }
        return err;
    }

    /*
     * DOUT is pulled up, and the direction matters.
     *
     * The NAU7802's DRDY is active high, so that driver pulls it *down* -- a pin
     * that turns out not to be wired then reads "no data" forever and times out
     * instead of looking permanently ready. The rule is to bias the ready line
     * toward "nothing available"; DOUT is active low, so here the same rule
     * means pulling up. A pull-down would be the worst option available: an
     * unconnected pin would read ready forever, clock 24 zeros out of nothing,
     * and report a clean 0 that looks exactly like an empty scale.
     */
    const gpio_config_t dout_config = {
        .pin_bit_mask = BIT64(dout),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&dout_config);
    if (err != ESP_OK) {
        gpio_reset_pin((gpio_num_t)sck);
        if (report) {
            report->failed_stage = HX711_STAGE_CONFIGURE_DOUT;
        }
        return err;
    }

    /* Power-cycle into a known state: this is the part's only reset. */
    pad_write(sck, 1);
    esp_rom_delay_us(HX711_POWERDOWN_HOLD_US);
    pad_write(sck, 0);

    handle->ready = true;
    handle->powered_down = false;
    handle->mode = HX711_MODE_A128;
    handle->last_a_mode = HX711_MODE_A128;
    handle->scale = (hx711_scale_t){0};
    handle->measured_sps = 0.0;
    handle->worst_high_us = 0;
    handle->stretched_bursts = 0;

    /*
     * DOUT falling is the first half of the proof that a part is there. A
     * floating pin cannot do it -- it is pulled up -- and neither can one
     * shorted low, which never read high to begin with. The second half is
     * inside read_one(): DOUT must come back high after the burst, which only
     * the clock reaching the device can cause.
     */
    err = wait_ready(handle, HX711_READY_TIMEOUT_MS, false);
    if (err != ESP_OK) {
        handle->ready = false;
        if (report) {
            report->failed_stage = HX711_STAGE_FIRST_READY;
        }
        return err;
    }

    hx711_change_report_t change = {0};
    err = apply_mode(handle, opts->mode, &change);
    if (report) {
        report->mode = change.mode;
        report->discards = change.discards;
        report->settling_conversions = change.settling_conversions;
    }
    if (err != ESP_OK) {
        handle->ready = false;
        if (report) {
            report->failed_stage = HX711_STAGE_SET_MODE;
        }
        return err;
    }

    /* Fatal, not best-effort: this is more conversions the part has to answer,
     * and letting it fail quietly would report success on a part that stopped
     * responding halfway through bring-up. */
    double sps = 0.0;
    err = hx711_measure_rate(handle, &sps);
    if (err != ESP_OK) {
        handle->ready = false;
        if (report) {
            report->failed_stage = HX711_STAGE_MEASURE_RATE;
        }
        return err;
    }
    if (report) {
        report->rate_measured = true;
        report->sps = sps;
    }

    /* One real measurement, so bring-up does not report success on a part that
     * answers the protocol and returns nothing. */
    hx711_stats_t first = {0};
    err = hx711_priv_read_average(handle, 5, &first);
    if (report) {
        report->first_reading = first;
    }
    if (err != ESP_OK) {
        handle->ready = false;
        if (report) {
            report->failed_stage = HX711_STAGE_FIRST_READING;
        }
        return err;
    }

    /*
     * Last, after the part has answered a real batch. Every failure above
     * clears `ready`, so applying the factor earlier would leave a scale
     * sitting on a part this call just disowned.
     */
    if (opts->set_scale) {
        err = hx711_set_scale(handle, opts->counts_per_unit);
        if (err != ESP_OK) {
            handle->ready = false;
            if (report) {
                report->failed_stage = HX711_STAGE_SET_SCALE;
            }
            return err;
        }
        if (report) {
            report->scale_supplied = true;
        }
    }

    return ESP_OK;
}

esp_err_t hx711_get_mode(hx711_handle_t handle, hx711_mode_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = handle->mode;
    return ESP_OK;
}

esp_err_t hx711_set_mode(hx711_handle_t handle, hx711_mode_t mode,
                         hx711_change_report_t *report)
{
    if (report) {
        *report = (hx711_change_report_t){0};
    }
    esp_err_t err = require_ready(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (!mode_valid(mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    return apply_mode(handle, mode, report);
}

esp_err_t hx711_set_channel(hx711_handle_t handle, hx711_channel_t channel,
                            hx711_change_report_t *report)
{
    if (report) {
        *report = (hx711_change_report_t){0};
    }
    esp_err_t err = require_ready(handle);
    if (err != ESP_OK) {
        return err;
    }

    hx711_mode_t requested;
    if (channel == HX711_CHANNEL_A) {
        /* Channel A has two gains, and naming the channel does not say which;
         * return to whichever was last in use. */
        requested = handle->last_a_mode;
    } else if (channel == HX711_CHANNEL_B) {
        requested = HX711_MODE_B32;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return apply_mode(handle, requested, report);
}

/*
 * Enter power-down by holding PD_SCK high. Returning it low resets the device,
 * which is also the only reset this part has.
 */
esp_err_t hx711_power_down(hx711_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    pad_write(handle->sck_pin, 1);
    handle->powered_down = true;
    return ESP_OK;
}

esp_err_t hx711_power_up(hx711_handle_t handle, hx711_change_report_t *report)
{
    if (report) {
        *report = (hx711_change_report_t){0};
    }
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    pad_write(handle->sck_pin, 0);
    handle->powered_down = false;

    esp_err_t err = wait_ready(handle, HX711_READY_TIMEOUT_MS, false);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * "After a reset or power-down event, input selection is default to Channel
     * A with a gain of 128", so the silicon has lost whatever was configured
     * even though this driver still remembers it. Re-applying is what puts the
     * two back in agreement.
     *
     * Deliberately passing the mode this driver already holds rather than
     * rewinding it to A/128 first: the gain is the same one the tare and scale
     * were captured at, so they still apply, and apply_mode() drops them only
     * when the setting really changes. It re-selects either way, because the
     * pulse count goes out on every burst.
     */
    return apply_mode(handle, handle->mode, report);
}

esp_err_t hx711_get_status(hx711_handle_t handle, hx711_status_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (hx711_status_t){0};
    out->ready = handle->ready;
    out->dout_gpio = handle->dout_pin;
    out->sck_gpio = handle->sck_pin;
    out->powered_down = handle->powered_down;
    out->mode = handle->mode;
    out->rate_measured = handle->measured_sps > 0.0;
    out->sps = handle->measured_sps;
    out->worst_high_us = handle->worst_high_us;
    out->stretched_bursts = handle->stretched_bursts;
    out->scale = handle->scale;

    /* The data sheet says nothing about what DOUT does while the part is
     * powered down, so do not report a level that would mean nothing. */
    if (!handle->ready || handle->powered_down) {
        out->dout_level = -1;
    } else {
        out->dout_level = pad_level(handle->dout_pin);
    }
    out->sck_level = handle->ready ? pad_level(handle->sck_pin) : -1;

    return ESP_OK;
}
