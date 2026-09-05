/*
 * NAU7802 register map and driver-internal state.
 *
 * Addresses, bit positions and encodings come from the data sheet, revision
 * 1.5, section 10 (Summary Device Register Map). Not a public header: a
 * consumer that needs a register number should be asking for a driver call
 * instead.
 */
#ifndef NAU7802_PRIV_H
#define NAU7802_PRIV_H

#include "esp_bit_defs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nau7802.h"

#define REG_PU_CTRL 0x00
#define REG_CTRL1   0x01
#define REG_CTRL2   0x02
/*
 * Each channel has its own offset and gain calibration registers, and the
 * internal calibration writes whichever set CTRL2.CHS currently selects. That
 * is why switching channels needs its own calibration rather than inheriting
 * the other channel's -- and why these are worth reading back: CAL_ERR is one
 * bit, whereas OCAL is the offset in ADC counts the device actually chose.
 */
#define REG_OCAL1_B2 0x03 /* 3 bytes, sign-magnitude 24-bit, channel 1 */
#define REG_GCAL1_B3 0x06 /* 4 bytes, 0x00800000 == unity, channel 1 */
#define REG_OCAL2_B2 0x0A /* 3 bytes, sign-magnitude 24-bit, channel 2 */
#define REG_GCAL2_B3 0x0D /* 4 bytes, 0x00800000 == unity, channel 2 */
#define REG_ADCO_B2  0x12

/* GCAL reads 0x00800000 for a gain correction of exactly 1.0. */
#define GCAL_UNITY 8388608.0

#define REG_PGA        0x1B
#define REG_POWER_CTRL 0x1C
#define REG_DEVICE_REV 0x1F

/* REG0x1B[6] LDOMODE. See the note on nau7802_set_ldomode(). */
#define PGA_LDOMODE BIT(6)

/* REG0x1C[7] PGA_CAP_EN. See the note on nau7802_set_pga_cap(). */
#define POWER_PGA_CAP_EN BIT(7)

/*
 * REG0x15 = 0x30, written unconditionally at power-up.
 *
 * This is not a setting, it is part of the prescribed bring-up. Section 11.10
 * gives REG_CHPS[5:4] -- the CLK_CHP chopper clock -- exactly one non-Reserved
 * encoding, '11' ("turned off, high ('1') state"), and section 9.1 Power-On
 * Sequencing step 4 spells out the write as a whole byte:
 *
 *   4. At this point, all appropriate device selections and configuration can
 *      be made.
 *        a. For example R0x00 = 0xAE
 *        b. R0x15 = 0x30
 *
 * Both widely used Arduino drivers do this; SparkFun's cites 9.1 by name. This
 * driver not doing it was the outlier, and it cost six bits. Measured with
 * inputs shorted at mid-rail, internal LDO 3.0 V, gain 128, 10 SPS, radio off:
 *
 *              CHPS 0        CHPS 3
 *   sd          3883          59.9
 *   p-p        22604           324
 *   ENOB        12.1          18.1
 *   input     5425 nV         84 nV
 *
 * 65x on RMS, 70x peak-to-peak, reproducible on every toggle. The quiet state
 * is a live converter, not the silent dead one an unpowered AVDD gives: OCAL
 * read -467, samples varied, and noise still tracked the sample rate.
 *
 * There is no way to change this and there should not be. Every other encoding
 * is Reserved, so the only alternative to the right value is a wrong one.
 *
 * Writing 0x30 was once observed railing the converter at negative full scale
 * on every rate. That observation was real and the conclusion drawn from it was
 * wrong: the write went in with no recalibration and no settling discard
 * afterwards, and a stale calibration against a changed modulator is exactly
 * what a railed reading looks like. The chopper sits at the PGA output, which
 * is also why the noise it caused was flat in counts across gain 1 to 128 and
 * why nothing upstream of the input pins ever moved it.
 *
 * Written as a whole byte, the form section 9.1 uses. That also zeroes
 * ADC_VCM[3:2] and REG_CHP[1:0], but zero is their reset value and neither is
 * useful here: ADC_VCM's extended common mode options all require PGA bypass
 * mode. The write is read back to confirm it took, which section 11.10 permits
 * so long as REG0x1B[7] RD_OTP_SEL is 0 -- the default, and nothing here
 * changes it. Set that bit and REG0x15 returns OTP[32:24] instead.
 */
#define REG_ADC_CTRL      0x15
#define ADC_CHPS_MASK     0x30
#define ADC_CHPS_SHIFT    4
#define ADC_CTRL_POWER_ON 0x30 /* section 9.1 step 4b, verbatim */

/* REG0x00 PU_CTRL. CR and PUR are read-only status. */
#define PU_CTRL_RR    BIT(0) /* register reset, level triggered */
#define PU_CTRL_PUD   BIT(1) /* power up digital */
#define PU_CTRL_PUA   BIT(2) /* power up analog */
#define PU_CTRL_PUR   BIT(3) /* power up ready (RO) */
#define PU_CTRL_CS    BIT(4) /* cycle start */
#define PU_CTRL_CR    BIT(5) /* cycle ready: ADC data available (RO) */
#define PU_CTRL_OSCS  BIT(6)
#define PU_CTRL_AVDDS BIT(7) /* 1 = internal LDO, 0 = AVDD pin */

/* REG0x01 CTRL1: GAINS[2:0], VLDO[5:3]. */
#define CTRL1_GAINS_SHIFT 0
#define CTRL1_GAINS_MASK  0x07
#define CTRL1_VLDO_SHIFT  3
#define CTRL1_VLDO_MASK   0x38

/* REG0x02 CTRL2: CALMOD[1:0], CALS, CAL_ERR, CRS[6:4], CHS. */
#define CTRL2_CALMOD_MASK 0x03
#define CTRL2_CALS        BIT(2)
#define CTRL2_CAL_ERR     BIT(3)
#define CTRL2_CRS_SHIFT   4
#define CTRL2_CRS_MASK    0x70
/*
 * CHS is bit 7, the top of the register -- bit 0 is CALMOD[0]. Getting this
 * wrong does not fail loudly: writing bit 0 selects a calibration mode instead
 * of a channel, run_calibration() then clears CALMOD back to 00, so the channel
 * silently never changes and every reading comes from channel 1 no matter what
 * was asked for. Reading the same wrong bit back agrees with itself throughout.
 */
#define CTRL2_CHS BIT(7)

/* CALMOD 00 = internal offset calibration, the one to run at power-up. */
#define CALMOD_OFFSET_INTERNAL 0x00

/*
 * How close to the rail counts as saturated.
 *
 * This used to be FULL_SCALE - 1, i.e. the exact end code, and it never once
 * fired. A saturated sigma-delta does not sit on its final code: the internal
 * offset calibration is subtracted from the result, and the modulator does not
 * hard-clip. Measured on an unbalanced bridge that was flat against the
 * negative rail with a spread of 1-3 counts across fifty conversions -- as
 * unambiguous as saturation gets -- the readings were -8340740, -8347645,
 * -8349211 and -8360097. That is 99.43% to 99.66% of full scale, and 28511 to
 * 47868 counts short of a threshold that demanded 99.999988%.
 *
 * 0.99 catches all four. 0.995 catches one and 0.999 none, so this is the right
 * order of magnitude rather than an arbitrary round number. The check is
 * advisory -- the data is still returned -- so a false positive on a legitimate
 * reading above 99% of full scale costs the caller one line of text.
 */
#define ADC_SATURATION_FRACTION 0.99

#define XFER_TIMEOUT_MS 1000

/* Worst case is 10 SPS, i.e. 100 ms per conversion; allow generous margin so a
 * misconfigured rate reports a timeout rather than hanging. */
#define CONVERSION_TIMEOUT_MS  1000
#define CALIBRATION_TIMEOUT_MS 2000

/*
 * What to throw away after the analog path changes.
 *
 * One conversion is stale rather than merely unsettled. The device holds the
 * last result in ADCO until it is read, and neither the calibration nor the
 * register write that prompted it clears that: DRDY stays high, CR stays set,
 * and the next read therefore returns immediately with a conversion that
 * finished under the *old* gain, rate or channel. It is not a bad sample, it is
 * the previous configuration's good sample, which is worse -- it looks right.
 *
 * The rest is filter settling. The converter is sigma-delta, so its output
 * depends on several preceding modulator cycles; three output periods clears
 * the history either side of a step. That is 300 ms at 10 SPS and under 10 ms
 * at 320, so it is charged at the rate the part is actually running.
 */
#define STALE_CONVERSIONS    1
#define SETTLING_CONVERSIONS 3

/*
 * How long to let AVDD settle before calibrating against it.
 *
 * The internal regulator has to slew to VLDO and charge whatever capacitor the
 * board fits on AVDD, and AVDD is the converter's reference. Calibrating into a
 * moving reference produces an offset that is valid for a supply the part is no
 * longer running on. Generous rather than measured: this is paid once.
 */
#define ANALOG_SETTLE_MS 200

/* How far the calibration move must clear the combined uncertainty of the two
 * averages. Ten times is a scale factor good to 10%, which is the point below
 * which the number is not worth having. */
#define CALIBRATION_SIGMA 10.0

struct nau7802_dev_t {
    i2c_master_dev_handle_t dev;
    bool ready; /* nau7802_bring_up() has succeeded */

    /* -1 means no pin is wired up, in which case conversions are detected by
     * polling PU_CTRL.CR. */
    int drdy_gpio;
    SemaphoreHandle_t drdy_signal;

    nau7802_scale_t scale;
};

/* nau7802.c, shared with nau7802_scale.c. */
esp_err_t nau7802_priv_read_reg(nau7802_handle_t handle, uint8_t reg, uint8_t *value);

#endif /* NAU7802_PRIV_H */
