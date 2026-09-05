# hx711

Driver for the Avia Semiconductor HX711, a 24-bit ADC for weigh scales, used as
a load cell front end. Bit-banged on two pins, with cycle-accurate clock timing
and a check against the data sheet's T3 limit, plus the tare and scale
arithmetic that turns counts into whatever unit you calibrated against.

Pulse counts, timings and encodings come from the HX711 data sheet as published
at
<https://cdn.sparkfun.com/datasheets/Sensors/ForceFlex/hx711_english.pdf>
(retrieved 2026-08-26). That document carries no revision number or date
anywhere, so it can only be cited by source.

## Installing

```yaml
# your project's idf_component.yml
dependencies:
  hx711:
    git: https://github.com/benvonhandorf/esp_components.git
    path: hx711
    version: hx711-v0.1.0
```

## Using it

The driver owns its two pins between `hx711_bring_up()` and `hx711_delete()`,
and nothing else. There is no bus to share.

```c
#include "hx711.h"

hx711_handle_t hx;
const hx711_config_t cfg = { .dout_gpio = 10, .sck_gpio = 3 };
ESP_ERROR_CHECK(hx711_create(&cfg, &hx));

hx711_bringup_opts_t opts = { .mode = HX711_MODE_A128 };
hx711_bringup_report_t report;
if (hx711_bring_up(hx, &opts, &report) != ESP_OK) {
    /* report.failed_stage says which step, so you can name the right pin. */
    hx711_delete(hx);
    return;
}

hx711_stats_t stats;
hx711_tare(hx, 10, &stats);              /* with nothing on the cell */
hx711_calibration_t cal;
hx711_calibrate(hx, 100.0, 10, &stats, &cal);  /* with a 100-unit mass */

hx711_weight_t w;
if (hx711_weigh(hx, 10, &stats, &w) == ESP_OK) {
    printf("%.4f units +/- %.4f\n", w.units, w.uncertainty_units);
}
```

A new pin pair is a new instance: there is no `hx711_set_pins()`, so delete the
handle and create another.

## It returns facts, not text

`hx711_bring_up()` collapses a power-cycle, a mode selection, a rate measurement
and a proof batch behind one call, so it fills an `hx711_stage_t` naming which
of them failed. Without that, a DOUT that never falls and a clock that never
reaches the part come out as the same "bring-up failed" — and they are different
wires.

`hx711_read_average()` likewise separates two things that look alike:
`saturated` is advisory and the batch still comes back, while an all-identical
batch returns `ESP_ERR_HX711_STUCK_READING`, because an exactly repeating value
is a wiring diagnosis rather than a data-quality note.

## Things this part does that will not look like errors

- **Gain and channel are one setting, not two.** Table 3 lists exactly three
  combinations, selected by the number of trailing clock pulses: 25 for A/128,
  26 for B/32, 27 for A/64. Channel B's gain is fixed at 32 and channel A cannot
  be set to it.
- **Figure 2's third waveform is mislabelled** "CH.B Gain:64". Both Table 3 and
  the "Analog Inputs" prose on page 4 say 27 pulses select channel *A*. Taking
  the figure at face value inverts the gain mapping, and the resulting readings
  are self-consistent — the ratio just comes out at 0.5 instead of 2.
- **The data sheet's own reference driver contradicts its own Table 2.** The
  code on page 8 ends with `Count = Count ^ 0x800000`, which returns offset
  binary rather than the two's complement Table 2 specifies. Tare and calibrate
  would hide it, since the offset cancels and the scale is linear — but raw
  readings are nonsense and the rail check compares against the wrong numbers.
- **A change of channel or gain needs the settling discarded.** Table 2 gives
  400 ms at 10 SPS and 50 ms at 80 SPS, which is four output periods either
  way — so this driver counts conversions rather than milliseconds and settles
  correctly without knowing how the RATE strap is wired, which it cannot ask.
  A change that skips the discard is invisible: the next reading comes back at
  the previous gain, off by exactly a factor of two or four, and self-consistent.
- **A stretched clock pulse silently resets the part.** PD_SCK high for over
  60 µs powers the HX711 down mid-read; it comes back at channel A gain 128 and
  returns a believable number at a gain nobody asked for, with the tare and
  scale still applied. So each high phase is timed *individually* — not the
  burst total, because the low phase has no maximum at all and a burst can have
  a normal total while one phase inside it blew the limit.
  - Between 50 µs (where T3 ends) and 60 µs (where power-down is documented to
    begin) the data sheet says nothing. This driver treats anything past 50 µs
    as a failure rather than assuming the gap is safe.
- **`clock_burst()` must not touch flash.** It runs with interrupts masked, but
  code executing from flash can still stall on an instruction-cache miss, and a
  flash write elsewhere disables the cache outright for milliseconds — which is
  exactly how a high phase gets stretched. Hence `IRAM_ATTR`, no calls except
  `esp_rom_delay_us()` and the inlined pad accessors, and **no `.rodata`**,
  which is why the pulse count arrives as a parameter instead of being looked
  up in a table. Nothing in the build enforces this; it is a constraint on
  anyone editing that function.
- **PD_SCK must be released pulled *down*.** `gpio_reset_pin()` enables the
  pull-up — "for powersave reasons, the GPIO should not be floating" — which on
  this pin is not a default but a command: 60 µs of high powers the part down.
  Handing the pin back in its reset state therefore switches the part off behind
  the caller's back. `hx711_delete()` releases it pulled down instead, the only
  level at which the part keeps converting with nothing driving it.
- **DOUT must be pulled *up*.** The ready line is active low, so the bias has to
  point at "nothing available". A pull-down would be the worst option: an
  unconnected pin would read ready forever, clock 24 zeros out of nothing, and
  report a clean 0 that looks exactly like an empty scale.
- **The output rate is a strap on pin 15**, not a register — low is 10 SPS, high
  is 80 SPS. `hx711_measure_rate()` finds out which; `hx711_rate_classify()`
  puts a result in a band. A value in neither band usually means XI (pin 14) is
  not grounded, so the part is running from a crystal or external clock.
- **There is no ID register.** Nothing proves the part on the end of the wires
  is an HX711 — only that something drives DOUT and answers the clock.

### Verified on hardware

2026-08-26, against an HX711 wired to an ESP32-C3, DOUT on GPIO 10 and PD_SCK on
GPIO 3. With nothing changing, the reading tracks the gain setting:

| Setting | Pulses | Reading (20 samples) |
|---|---|---|
| A / 128 | 25 | −103,570 |
| A / 64  | 27 | −54,336 |
| B / 32  | 26 | −11,156 |

128/64 comes out at **1.91** against an ideal 2.00. The shortfall is not error:
solving the two readings for a gain-independent term gives about −5100 counts of
offset that does not scale, which is what the model predicts. What matters is
that the ratio is near two *and the right way up* — 25 pulses reads roughly
double 27. Had the mapping been inverted, or had Figure 2's mislabelling been
taken at face value, this would have come out at 0.5.

Channel B does not fit the same line, and should not: it is a different physical
input, usually connected to nothing.

Constants the data sheet does not actually state are marked `UNVERIFIED` where
they are defined.

## Portability

`src/hx711.c` includes **`esp_private/esp_clk.h`** for one call,
`esp_clk_cpu_freq()`, used to turn a cycle count into microseconds for the T3
diagnostic. Private IDF headers carry no API-stability promise across releases,
so this is the one thing to check when moving to a newer IDF. The manifest's
`idf: ">=5.3"` states the range this was verified against rather than a feature
it needs.

The public replacement is `esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, …)`
from `esp_clk_tree.h`, at the cost of an `esp_err_t` return. `cycles_to_us()`
runs once per burst and *outside* the critical section, so the extra cost is
irrelevant. The swap is deliberately not made here: it changes the code that
decides whether a high phase exceeded 50 µs, so it wants a bench re-verification
rather than only a build.

Everything else is portable across ESP targets. `soc/gpio_reg.h` is a public
per-target header, and the pad accessors already handle pins above 31 through
the `GPIO_OUT1_*` / `GPIO_IN1_REG` branches.

## License

Not yet declared. The repository this component lives in has no LICENSE file, so
`idf_component.yml` carries no `license:` key either; that is a gap to close
before the first tag, not a claim that the code is unencumbered.
