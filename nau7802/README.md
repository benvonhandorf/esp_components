# nau7802

Driver for the Nuvoton NAU7802, a 24-bit bridge ADC usually found in front of a
load cell. Register-level control, DRDY interrupt support, and the tare and
scale arithmetic that turns counts into whatever unit you calibrated against.

The device address is `0x2A`, fixed in silicon — the part has no address pins,
so there can be one per bus.

## Installing

```yaml
# your project's idf_component.yml
dependencies:
  nau7802:
    git: https://github.com/benvonhandorf/esp_components.git
    path: nau7802
    version: nau7802-v0.2.0
    version: nau7802-v0.2.0
```

Requires ESP-IDF 5.3 or later, for the `i2c_master` driver.

## Using it

The driver does not own the I2C bus and does not create a device on it. You pass
in a device handle you made, and you keep owning it — so the same bus can carry
other parts, managed however your application already manages them.

```c
#include "nau7802.h"

i2c_master_dev_handle_t dev;
const i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = NAU7802_I2C_ADDRESS,
    .scl_speed_hz    = 100000,
};
ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

nau7802_handle_t nau;
const nau7802_config_t cfg = {
    .dev       = dev,
    .drdy_gpio = 7,   /* or -1 to poll PU_CTRL.CR over I2C */
};
ESP_ERROR_CHECK(nau7802_create(&cfg, &nau));

const nau7802_bringup_opts_t opts = {
    .use_internal_ldo = true,
    .ldo              = NAU7802_LDO_3V0,
    .set_gain         = true,
    .gain             = NAU7802_GAIN_128,
};
nau7802_bringup_report_t report;
ESP_ERROR_CHECK(nau7802_bring_up(nau, &opts, &report));

nau7802_stats_t stats;
ESP_ERROR_CHECK(nau7802_read_average(nau, 10, &stats));
printf("%.1f counts +/- %.1f\n", stats.mean, stats.stderr_mean);
```

To weigh things, tare with the cell empty, calibrate against a known mass, and
then ask for weights:

```c
nau7802_tare(nau, 100, NULL);
/* ... put a 100 g mass on the cell ... */
nau7802_calibration_t cal;
if (nau7802_calibrate(nau, 100.0, 100, NULL, &cal) == ESP_OK) {
    nau7802_weight_t w;
    nau7802_weigh(nau, 20, NULL, &w);
    printf("%.4f +/- %.4f\n", w.units, w.uncertainty_units);
}
```

A product that was calibrated in the factory has no reason to repeat that on
every boot. Compile the factor in and supply it to bring-up, which ties it to
the gain set in the same call:

```c
const nau7802_bringup_opts_t opts = {
    .use_internal_ldo = true,
    .ldo              = NAU7802_LDO_3V0,
    .set_gain         = true,
    .gain             = NAU7802_GAIN_128,
    .set_scale        = true,
    .counts_per_unit  = 214.69999999999999,  /* bench run, gain x128 */
};
```

Only the factor. The tare is still measured at runtime — `nau7802_tare()` with
the cell empty, before the first weight — because it is the bridge's own zero
and no constant can stand in for it. `nau7802_weigh()` reports `tare_taken`
false until it has been, and its reading until then is the unloaded offset, not
a weight.

If your bus handle or device handle is ever recreated — a bus torn down and
rebuilt, a device-handle cache recycled — call `nau7802_set_device()` with the
new one. The driver holds the handle and does not otherwise notice.

## It returns facts, not text

Nothing here prints. Calls return `esp_err_t` and fill small structs describing
what happened, so the caller decides what to say and in what form:

- `nau7802_bringup_report_t` names which stage of the power-up failed, and
  carries the device revision, the gain and the chopper setting **read back**
  rather than assumed.
- `nau7802_stats_t` carries the mean, the standard error of that mean, the
  extremes, and whether the converter was saturated (advisory — the data is
  still returned).
- `nau7802_change_report_t` says what a configuration change cost: whether the
  tare and scale were dropped, whether the dropped factor was one you supplied
  rather than one measured here, whether conversions had to be restarted, how
  many conversions were discarded.
- `nau7802_calibration_t` is filled whether a calibration succeeds or is refused,
  so a refusal can quote the move, the uncertainty and the PGA gain.

Three error codes distinguish refusals from bus failures:
`ESP_ERR_NAU7802_WITHIN_NOISE`, `ESP_ERR_NAU7802_CAL_FAILED`,
`ESP_ERR_NAU7802_TOO_FEW_SAMPLES`.

## Things this part does that will not look like errors

Most of the work in this driver is not the register access. It is the handful of
behaviours that produce plausible numbers rather than failures, each of which
cost a debugging session to find. They are documented at the point they happen
in the source; in summary:

- **`REG0x15` must be written `0x30` at power-up, and it is worth six bits.**
  Section 9.1 step 4b prescribes the write; §11.10 gives `REG_CHPS[5:4]` exactly
  one non-Reserved encoding. Measured with inputs shorted at mid-rail, gain 128,
  10 SPS: 3883 counts RMS with the chopper left at its power-up value against
  59.9 with it written — 12.1 effective bits against 18.1. `nau7802_bring_up()`
  does this unconditionally and reads it back.
- **VLDO is written before AVDDS.** The reset returns `CTRL1` to `0x00`, and
  VLDO `000` is 4.5 V, the *top* of the range. Enabling the regulator first
  brings AVDD up at 4.5 V and only then winds it down, putting an overvoltage on
  AVDD — and on whatever the board bridges from it — for one I2C transaction.
- **Bringing the part up resets the gain to x1.** Nothing downstream looks
  wrong: the converter comes up, the offset calibration passes, readings are
  simply 128× smaller, which puts a load cell among the noise. The bring-up
  report carries the gain read back so a caller can say so.
- **`CTRL2.CHS` is bit 7, not bit 0.** Bit 0 is `CALMOD[0]`, so getting it wrong
  selects a calibration mode instead of a channel, the calibration then clears
  it, and every reading comes from channel 1 while the read-back agrees that the
  channel changed.
- **`OCAL` is sign-magnitude, not two's complement**, unlike the result
  registers a few addresses away. Decoded the wrong way, every negative offset
  reads as pinned near negative full scale.
- **The result registers tear.** There is no shadow register and no read latch:
  the converter rewrites all three bytes at end-of-conversion regardless of an
  I2C transaction in flight. A read straddling that moment returns one
  conversion's top byte on the next one's low bytes — near zero, a ±65,500
  outlier on an input that is not moving. Wiring DRDY starts the read
  microseconds after the registers are written; polling `CR` runs on the
  FreeRTOS tick and starts it at an unknown phase.
- **DRDY is configured with a pull-down.** The device drives it push-pull, so
  the pull is not there to hold the line — it is there so that a pin which is
  *not* connected to DRDY times out instead of reading ready forever.
- **Every change to the analog path needs a recalibration, a restart and a
  flush.** One conversion after a change is stale rather than unsettled: the
  device holds the last result until it is read, so the next read returns a good
  sample taken under the *old* configuration. The driver discards that one plus
  three output periods of filter settling, and drops the host-side tare and
  scale, which were measured at the old gain.
- **A converter pinned at a rail does not sit on its final code.** Measured on a
  bridge unambiguously against the negative rail, readings came in at 99.43% to
  99.66% of full scale. Saturation is therefore flagged at 99%, not at the end
  code.
- **The calibration guard tests the uncertainty of the means, not the spread of
  the samples.** Peak-to-peak spread *grows* with the sample count while the
  uncertainty of a mean falls as 1/sqrt(n), so a guard written against the
  spread gets harder to satisfy the more you average — making "take more
  samples" the wrong advice and calibration unreachable at any sample count.

## License

Not yet declared. The repository this component lives in has no LICENSE file, so
`idf_component.yml` carries no `license:` key either; that is a gap to close
before the first tag, not a claim that the code is unencumbered.
