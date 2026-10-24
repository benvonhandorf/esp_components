# es8311

Everest ES8311 mono audio codec: a DAC for a speaker amplifier and an ADC for an analog
or PDM microphone. Controlled over I2C, audio over I2S, with the host as I2S master.

```c
es8311_handle_t codec;
es8311_create(i2c_device_handle(ES8311_I2C_ADDR_DEFAULT), &codec);

es8311_report_t report;
es8311_format_t fmt = {
    .rate_hz = 8000, .bits = 16,
    .clk_src = ES8311_CLK_BCLK,        /* no MCLK pin */
    .clk_hz  = 8000 * 16 * 2,          /* the bit clock the I2S driver produces */
};
if (es8311_start(codec, &fmt, &report) != ESP_OK) {
    /* report.failed_stage and report.reg say what to fix */
}
es8311_set_dac_volume(codec, es8311_dac_code_from_db_x10(-200));  /* -20 dB */
es8311_set_mute(codec, false);
```

## No MCLK pin

The part runs its converters from 256 x the sample rate. With an MCLK pin that is a
division of whatever the host supplies. Without one, the part takes its bit clock as the
master clock -- and at 8 kHz, 16-bit stereo, that is 256 kHz. The pre-multiplier (x2, x4,
x8) exists for this: x8 gives 2.048 MHz. `es8311_clock_plan()` finds the divider and
multiplier for any input that can reach 256 x fs, and says
`ESP_ERR_ES8311_UNSUPPORTED_CLOCK` for one that cannot.

Rates are 8-48 kHz. The 384 x and 512 x fs configurations some MCLK rates allow are not
used.

## Starts silent

`es8311_start()` leaves the DAC muted and at the lowest volume code. The volume scale is
half-dB steps with 0 dB at `ES8311_DAC_CODE_0DB` (0xBF); codes above it are gain up to
+32 dB, which this API does not hand out by accident -- `es8311_dac_code_from_db_x10()`
stops at 0 dB. Write a raw code with `es8311_set_dac_volume()` if you want the gain.

## It does not own the bus

`dev` is an I2C device handle the caller created and owns, and may be NULL at creation;
bus calls return `ESP_ERR_INVALID_STATE` until `es8311_set_device()` supplies one.

## Tests

```sh
make -C es8311/test
```
