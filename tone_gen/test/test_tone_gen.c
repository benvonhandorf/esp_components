#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tone_gen.h"

static int failures;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
            failures++;                                               \
        }                                                             \
    } while (0)

/* Amplitude of `hz` in x[0..n), by Goertzel, in the same units as x. */
static double amplitude(const int16_t *x, size_t n, double hz, double rate)
{
    double w = 2.0 * M_PI * hz / rate;
    double coeff = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * sqrt(power > 0.0 ? power : 0.0) / (double)n;
}

/* As amplitude(), through a Hann window. A rectangular window's sidelobes
 * leak about -25 dB between 697 and 770 Hz over a 60 ms look, which says
 * something about the measurement and nothing about the signal. */
static double amplitude_hann(const int16_t *x, size_t n, double hz, double rate)
{
    double w = 2.0 * M_PI * hz / rate;
    double coeff = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double win = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(n - 1));
        wsum += win;
        double s = x[i] * win + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * sqrt(power > 0.0 ? power : 0.0) / wsum;
}

static double rms(const int16_t *x, size_t n)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)x[i] * x[i];
    }
    return sqrt(sum / (double)n);
}

/* Render everything the generator produces, in blocks of `block` samples. */
static size_t render(tone_gen_t *g, int16_t *out, size_t cap, size_t block)
{
    size_t total = 0;
    while (total < cap) {
        size_t want = block < cap - total ? block : cap - total;
        size_t got = tone_gen_fill(g, out + total, want);
        total += got;
        if (got < want) {
            break;
        }
    }
    return total;
}

/* Frequency of x[lo..lo+len) from the spacing of its rising zero crossings:
 * first to last, so the estimate is limited by one sample, not by one cycle. */
static double crossing_hz(const int16_t *x, size_t lo, size_t len, double rate)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = lo + 1; i < lo + len; i++) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            double t = (double)(i - 1) + (double)(-x[i - 1]) / (double)(x[i] - x[i - 1]);
            if (first < 0.0) first = t;
            last = t;
            count++;
        }
    }
    if (count < 3) return 0.0;
    return (count - 1) * rate / (last - first);
}

static void write_wav(const char *name, const int16_t *x, size_t n, uint32_t rate)
{
    if (!getenv("WAV")) {
        return;
    }
    FILE *f = fopen(name, "wb");
    if (!f) {
        return;
    }
    uint32_t data = (uint32_t)(n * 2), riff = 36 + data, brate = rate * 2;
    uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    uint32_t sixteen = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(x, 2, n, f);
    fclose(f);
}

static tone_gen_config_t tone(uint32_t rate, double hz, double seconds, int peak, uint16_t ramp_ms)
{
    tone_gen_config_t c = {
        .sample_rate_hz = rate, .start_hz = hz, .end_hz = hz,
        .duration_samples = (uint64_t)llround(seconds * rate),
        .peak = (int16_t)peak, .ramp_ms = ramp_ms,
    };
    return c;
}

/* ---- oscillator ---------------------------------------------------- */

static void test_oscillator_accuracy(void)
{
    int worst = 0;
    /* Every 7th-bit-spaced phase plus fractional positions between table points. */
    for (uint64_t p = 0; p < (1ull << 32); p += 4099) {
        double ideal = 32767.0 * sin(2.0 * M_PI * (double)p / 4294967296.0);
        int err = abs((int)lrint(ideal) - tone_gen_sine((uint32_t)p));
        if (err > worst) {
            worst = err;
        }
    }
    CHECK(worst <= 2, "worst oscillator error %d LSB, wanted <= 2", worst);

    CHECK(tone_gen_sine(0) == 0, "sine(0)");
    CHECK(tone_gen_sine(0x40000000u) == 32767, "sine(pi/2)");
    CHECK(abs(tone_gen_sine(0x80000000u)) <= 1, "sine(pi)");
    CHECK(tone_gen_sine(0xC0000000u) == -32767, "sine(3pi/2)");
}

static void test_phase_steps(void)
{
    CHECK(tone_gen_phase_step(6000.0, 48000) == 0x20000000u, "6 kHz at 48 kHz is an eighth of a turn");
    CHECK(tone_gen_phase_step_int(1000, 8000) == 0x20000000u, "1 kHz at 8 kHz");
    CHECK(tone_gen_phase_step_int(697, 8000) == tone_gen_phase_step(697.0, 8000),
          "integer and double steps agree on 697 Hz");
    CHECK(tone_gen_phase_step_int(1633, 8000) == tone_gen_phase_step(1633.0, 8000),
          "integer and double steps agree on 1633 Hz");
}

static void test_oscillator_purity(void)
{
    /* 1 kHz at 48 kHz, whole cycles: harmonics should sit well below the tone. */
    enum { N = 48000 };
    static int16_t x[N];
    uint32_t step = tone_gen_phase_step(1000.0, 48000), phase = 0;
    for (int i = 0; i < N; i++) {
        x[i] = tone_gen_sine(phase);
        phase += step;
    }
    double fund = amplitude(x, N, 1000.0, 48000);
    CHECK(fund > 32700.0 && fund < 32800.0, "fundamental amplitude %.1f", fund);
    for (int h = 2; h <= 10; h++) {
        double a = amplitude(x, N, 1000.0 * h, 48000);
        double dbc = 20.0 * log10((a + 1e-9) / fund);
        CHECK(dbc < -85.0, "harmonic %d at %.1f dBc, wanted below -85", h, dbc);
    }
}

/* ---- tone ----------------------------------------------------------- */

static void test_tone_frequency_and_length(void)
{
    static const uint32_t rates[] = {8000, 16000, 44100, 48000};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        uint32_t rate = rates[r];
        double hz = rate / 8.0 + 13.0;           /* not a divisor of anything */
        tone_gen_config_t c = tone(rate, hz, 0.5, 16000, 5);
        tone_gen_t g;
        CHECK(tone_gen_init(&g, &c) == ESP_OK, "init at %u", (unsigned)rate);

        static int16_t x[48000];
        size_t n = render(&g, x, sizeof(x) / sizeof(x[0]), 240);
        CHECK(n == c.duration_samples, "rate %u: length %zu, wanted %llu", (unsigned)rate, n,
              (unsigned long long)c.duration_samples);
        CHECK(tone_gen_done(&g), "done after the last sample");
        CHECK(tone_gen_fill(&g, x, 10) == 0, "a finished generator stays finished");

        /* Measure the middle, clear of the ramps. */
        size_t lo = n / 4, len = n / 2;
        double a = amplitude(x + lo, len, hz, rate);
        CHECK(fabs(a - 16000.0) < 60.0, "rate %u: amplitude %.1f, wanted 16000", (unsigned)rate, a);
        double off = amplitude(x + lo, len, hz * 1.1, rate);
        CHECK(off < 300.0, "rate %u: energy %.1f at the wrong frequency", (unsigned)rate, off);
    }
}

static void test_ramps(void)
{
    tone_gen_config_t c = tone(48000, 1000.0, 0.2, 20000, 5);
    tone_gen_t g;
    tone_gen_init(&g, &c);
    static int16_t x[9600];
    size_t n = render(&g, x, 9600, 240);
    CHECK(n == 9600, "length");

    CHECK(abs(x[0]) <= 1, "starts at zero, got %d", x[0]);
    CHECK(abs(x[n - 1]) <= 40, "ends near zero, got %d", x[n - 1]);

    /* Monotone envelope: the peak of each cycle grows through the opening ramp. */
    int peak_early = 0, peak_late = 0;
    for (int i = 0; i < 48; i++) {
        if (abs(x[i]) > peak_early) peak_early = abs(x[i]);
    }
    for (int i = 240; i < 288; i++) {
        if (abs(x[i]) > peak_late) peak_late = abs(x[i]);
    }
    CHECK(peak_early < peak_late / 2, "opening ramp: early peak %d vs steady %d", peak_early, peak_late);
    CHECK(peak_late > 19900, "steady state reaches the peak, got %d", peak_late);

    /* No sample-to-sample jump anywhere larger than the signal itself allows. */
    int worst = 0;
    for (size_t i = 1; i < n; i++) {
        int d = abs(x[i] - x[i - 1]);
        if (d > worst) worst = d;
    }
    CHECK(worst < 2 * 20000 * 2 * M_PI * 1000.0 / 48000.0 / 2 + 50, "step %d is a click", worst);
    write_wav("tone.wav", x, n, 48000);
}

static void test_hard_edges_and_zero_peak(void)
{
    tone_gen_config_t c = tone(8000, 500.0, 0.1, 10000, 0);
    tone_gen_t g;
    CHECK(tone_gen_init(&g, &c) == ESP_OK, "init with no ramp");
    int16_t x[800];
    CHECK(render(&g, x, 800, 100) == 800, "length with no ramp");

    c = tone(8000, 500.0, 0.1, 0, 5);
    tone_gen_init(&g, &c);
    render(&g, x, 800, 100);
    CHECK(rms(x, 800) == 0.0, "zero peak is silence");
}

static void test_short_signal_clamps_ramp(void)
{
    /* 3 ms of tone with a 5 ms ramp: the two edges would overlap. */
    tone_gen_config_t c = tone(48000, 1000.0, 0.003, 20000, 5);
    tone_gen_t g;
    tone_gen_init(&g, &c);
    int16_t x[256];
    size_t n = render(&g, x, 256, 64);
    CHECK(n == 144, "length %zu", n);
    int peak = 0;
    for (size_t i = 0; i < n; i++) if (abs(x[i]) > peak) peak = abs(x[i]);
    CHECK(peak > 5000, "a clamped ramp still produces a tone (peak %d)", peak);
}

static void test_block_size_independence(void)
{
    static const size_t blocks[] = {1, 7, 240, 512};
    static int16_t ref[24000], got[24000];

    tone_gen_config_t c = tone(48000, 1234.5, 0.4, 12000, 5);
    tone_gen_t g;
    tone_gen_init(&g, &c);
    size_t n = render(&g, ref, 24000, 24000);

    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        tone_gen_init(&g, &c);
        size_t m = render(&g, got, 24000, blocks[b]);
        CHECK(m == n && memcmp(ref, got, n * 2) == 0, "tone differs at block size %zu", blocks[b]);
    }

    /* The same for a sweep, which has an update boundary to fall across. */
    c.end_hz = 6000.0;
    c.logarithmic = true;
    tone_gen_init(&g, &c);
    n = render(&g, ref, 24000, 24000);
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        tone_gen_init(&g, &c);
        size_t m = render(&g, got, 24000, blocks[b]);
        CHECK(m == n && memcmp(ref, got, n * 2) == 0, "sweep differs at block size %zu", blocks[b]);
    }
}

/* ---- release -------------------------------------------------------- */

static void test_release_continuous(void)
{
    tone_gen_config_t c = tone(48000, 1000.0, 0.0, 20000, 5);   /* continuous */
    tone_gen_t g;
    tone_gen_init(&g, &c);
    static int16_t x[48000];

    size_t n = tone_gen_fill(&g, x, 4800);
    CHECK(n == 4800 && !tone_gen_done(&g), "a continuous tone does not end by itself");
    n = tone_gen_fill(&g, x, 4800);
    CHECK(n == 4800, "still going");

    tone_gen_release(&g);
    size_t tail = render(&g, x, 4800, 100);
    CHECK(tail == 240, "release ends in exactly the ramp (%zu samples)", tail);
    CHECK(tone_gen_done(&g), "done after the ramp");
    CHECK(abs(x[tail - 1]) <= 60, "ends near zero, got %d", x[tail - 1]);

    int worst = 0;
    for (size_t i = 1; i < tail; i++) {
        int d = abs(x[i] - x[i - 1]);
        if (d > worst) worst = d;
    }
    /* The steepest a 20000-peak 1 kHz sine goes at 48 kHz is 2618 per sample; a
     * fade can only make that smaller. */
    CHECK(worst <= 2618, "release steps %d is a click", worst);
}

static void test_release_without_ramp_and_during_ramp(void)
{
    tone_gen_config_t c = tone(8000, 500.0, 0.0, 10000, 0);
    tone_gen_t g;
    tone_gen_init(&g, &c);
    int16_t x[64];
    tone_gen_fill(&g, x, 64);
    tone_gen_release(&g);
    CHECK(tone_gen_done(&g) && tone_gen_fill(&g, x, 64) == 0, "no ramp: release ends at once");

    /* A finite tone released inside its own closing ramp is left alone. */
    c = tone(48000, 1000.0, 0.1, 10000, 5);
    tone_gen_init(&g, &c);
    static int16_t y[4800];
    tone_gen_fill(&g, y, 4700);
    tone_gen_release(&g);
    CHECK(tone_gen_fill(&g, y, 4800) == 100, "released late: the signal still ends on schedule");
}

/* ---- sweep ---------------------------------------------------------- */

static void test_sweep(void)
{
    enum { RATE = 48000, SECONDS = 2 };
    static int16_t x[RATE * SECONDS];
    tone_gen_config_t c = tone(RATE, 100.0, SECONDS, 15000, 5);
    c.end_hz = 6400.0;
    c.logarithmic = true;
    tone_gen_t g;
    CHECK(tone_gen_init(&g, &c) == ESP_OK, "init sweep");
    size_t n = render(&g, x, sizeof(x) / sizeof(x[0]), 240);
    CHECK(n == RATE * SECONDS, "sweep length %zu", n);

    /* Log sweep, 6 octaves in 2 s: at the midpoint it is at the geometric mean
     * (800 Hz); a quarter of the way through it is 100 * 2^1.5 = 283 Hz. */
    size_t window = 1920;   /* 40 ms */
    double mid = crossing_hz(x, n / 2 - window / 2, window, RATE);
    CHECK(fabs(mid - 800.0) < 800.0 * 0.05, "log midpoint at %.1f Hz, wanted 800", mid);
    double quarter = crossing_hz(x, n / 4 - window / 2, window, RATE);
    CHECK(fabs(quarter - 283.0) < 283.0 * 0.05, "log quarter point at %.1f Hz, wanted 283", quarter);
    double near_end = crossing_hz(x, n - 3 * window, window, RATE);
    CHECK(near_end > 5000.0 && near_end < 6500.0, "log end at %.1f Hz", near_end);

    write_wav("sweep_log.wav", x, n, RATE);

    /* Linear: 100 -> 6400 Hz, midpoint 3250 Hz, a quarter 1675 Hz. */
    c.logarithmic = false;
    tone_gen_init(&g, &c);
    render(&g, x, sizeof(x) / sizeof(x[0]), 240);
    double lin_mid = crossing_hz(x, n / 2 - window / 2, window, RATE);
    CHECK(fabs(lin_mid - 3250.0) < 3250.0 * 0.03, "linear midpoint at %.1f Hz, wanted 3250", lin_mid);
    double lin_q = crossing_hz(x, n / 4 - window / 2, window, RATE);
    CHECK(fabs(lin_q - 1675.0) < 1675.0 * 0.03, "linear quarter point at %.1f Hz, wanted 1675", lin_q);

    /* Current frequency tracks the sweep and is exact at the ends. */
    tone_gen_init(&g, &c);
    int16_t scratch[1];
    tone_gen_fill(&g, scratch, 1);
    CHECK(fabs(tone_gen_current_hz(&g) - 100.0) < 0.01, "starts at 100 Hz, is %.3f", tone_gen_current_hz(&g));
}

/* ---- arguments ------------------------------------------------------ */

static void test_invalid_arguments(void)
{
    tone_gen_t g;
    tone_gen_config_t c = tone(8000, 1000.0, 1.0, 1000, 5);

    CHECK(tone_gen_init(NULL, &c) == ESP_ERR_INVALID_ARG, "null generator");
    CHECK(tone_gen_init(&g, NULL) == ESP_ERR_INVALID_ARG, "null config");

    c.sample_rate_hz = 0;
    CHECK(tone_gen_init(&g, &c) == ESP_ERR_INVALID_ARG, "zero rate");

    c = tone(8000, 4000.0, 1.0, 1000, 5);
    CHECK(tone_gen_init(&g, &c) == ESP_ERR_INVALID_ARG, "frequency at Nyquist");
    c = tone(8000, 0.0, 1.0, 1000, 5);
    CHECK(tone_gen_init(&g, &c) == ESP_ERR_INVALID_ARG, "zero frequency");
    c = tone(8000, 1000.0, 1.0, -5, 5);
    CHECK(tone_gen_init(&g, &c) == ESP_ERR_INVALID_ARG, "negative peak");

    c = tone(8000, 100.0, 0.0, 1000, 5);
    c.end_hz = 200.0;
    CHECK(tone_gen_init(&g, &c) == ESP_ERR_INVALID_ARG, "a sweep needs a duration");
}

/* ---- DTMF ----------------------------------------------------------- */

static size_t render_dtmf(tone_gen_dtmf_t *g, int16_t *out, size_t cap, size_t block)
{
    size_t total = 0;
    while (total < cap) {
        size_t want = block < cap - total ? block : cap - total;
        size_t got = tone_gen_dtmf_fill(g, out + total, want);
        total += got;
        if (got < want) {
            break;
        }
    }
    return total;
}

static void test_dtmf_every_key(void)
{
    static const char keys[] = "123A456B789C*0#D";
    static const uint16_t lows[] = {697, 770, 852, 941};
    static const uint16_t highs[] = {1209, 1336, 1477, 1633};
    static const uint32_t rates[] = {8000, 16000, 48000};

    for (size_t r = 0; r < 3; r++) {
        uint32_t rate = rates[r];
        tone_gen_dtmf_config_t cfg = TONE_GEN_DTMF_CONFIG_DEFAULT(rate);
        tone_gen_dtmf_t g;
        CHECK(tone_gen_dtmf_init(&g, &cfg, keys) == ESP_OK, "init at %u", (unsigned)rate);

        static int16_t x[48000 * 4];
        size_t n = render_dtmf(&g, x, sizeof(x) / sizeof(x[0]), 240);
        size_t tone = rate / 10, gap = rate / 10;
        CHECK(n == 16 * tone + 15 * gap, "rate %u: length %zu", (unsigned)rate, n);
        CHECK(n == tone_gen_dtmf_total_samples(&cfg, keys), "total_samples agrees with what was rendered");
        CHECK(tone_gen_dtmf_done(&g), "done");

        int worst_peak = 0;
        for (size_t i = 0; i < n; i++) {
            if (abs(x[i]) > worst_peak) worst_peak = abs(x[i]);
        }
        CHECK(worst_peak <= cfg.low_peak + cfg.high_peak, "rate %u: peak %d exceeds the sum of the two tones",
              (unsigned)rate, worst_peak);

        for (int k = 0; k < 16; k++) {
            size_t start = (size_t)k * (tone + gap);
            /* The middle 60%: clear of the 3 ms edges. */
            size_t lo = start + tone / 5, len = tone * 3 / 5;
            int row = -1, col = -1;
            for (int a = 0; a < 4; a++) {
                for (int b = 0; b < 4; b++) {
                    if ("123A456B789C*0#D"[a * 4 + b] == keys[k]) { row = a; col = b; }
                }
            }
            double lo_amp = amplitude(x + lo, len, lows[row], rate);
            double hi_amp = amplitude(x + lo, len, highs[col], rate);
            CHECK(fabs(lo_amp - cfg.low_peak) < cfg.low_peak * 0.05, "rate %u key %c: low tone %.0f, wanted %d",
                  (unsigned)rate, keys[k], lo_amp, cfg.low_peak);
            CHECK(fabs(hi_amp - cfg.high_peak) < cfg.high_peak * 0.05, "rate %u key %c: high tone %.0f, wanted %d",
                  (unsigned)rate, keys[k], hi_amp, cfg.high_peak);

            double twist = 20.0 * log10(hi_amp / lo_amp);
            CHECK(fabs(twist - 2.0) < 0.4, "rate %u key %c: twist %.2f dB, wanted 2", (unsigned)rate, keys[k], twist);

            /* The six tones that are not this key's must be well down. */
            double weakest = fmin(amplitude_hann(x + lo, len, lows[row], rate),
                                  amplitude_hann(x + lo, len, highs[col], rate));
            for (int a = 0; a < 4; a++) {
                if (a == row) continue;
                double other = amplitude_hann(x + lo, len, lows[a], rate);
                CHECK(20.0 * log10(other / weakest + 1e-12) < -30.0, "rate %u key %c: %u Hz only %.1f dB down",
                      (unsigned)rate, keys[k], lows[a], 20.0 * log10(other / weakest));
            }
            for (int b = 0; b < 4; b++) {
                if (b == col) continue;
                double other = amplitude_hann(x + lo, len, highs[b], rate);
                CHECK(20.0 * log10(other / weakest + 1e-12) < -30.0, "rate %u key %c: %u Hz only %.1f dB down",
                      (unsigned)rate, keys[k], highs[b], 20.0 * log10(other / weakest));
            }

            /* The gap after every key but the last is exact silence. */
            if (k < 15) {
                size_t gstart = start + tone;
                size_t nonzero = 0;
                for (size_t i = gstart; i < gstart + gap; i++) if (x[i] != 0) nonzero++;
                CHECK(nonzero == 0, "rate %u: %zu non-zero samples in the gap after %c", (unsigned)rate, nonzero, keys[k]);
            }
        }
        if (rate == 8000) {
            write_wav("dtmf_8k.wav", x, n, rate);
        }
    }
}

static void test_dtmf_timing_options(void)
{
    tone_gen_dtmf_config_t cfg = TONE_GEN_DTMF_CONFIG_DEFAULT(8000);
    cfg.tone_ms = 50;
    cfg.gap_ms = 40;
    cfg.pause_ms = 500;
    static int16_t x[8000 * 4];
    tone_gen_dtmf_t g;

    /* tone, gap, pause, tone: the gap is scheduled before the pause. */
    CHECK(tone_gen_dtmf_init(&g, &cfg, "1,2") == ESP_OK, "init with a pause");
    size_t n = render_dtmf(&g, x, sizeof(x) / sizeof(x[0]), 77);
    size_t expect = 2 * 400 + 320 + 4000;   /* two keys, one gap, one pause */
    CHECK(n == expect, "1,2 is %zu samples, wanted %zu", n, expect);
    CHECK(n == tone_gen_dtmf_total_samples(&cfg, "1,2"), "total_samples with a pause");

    /* A single key has no gap at all. */
    tone_gen_dtmf_init(&g, &cfg, "5");
    CHECK(render_dtmf(&g, x, sizeof(x) / sizeof(x[0]), 64) == 400, "one key, no gap");

    /* Zero gap and zero ramp still dial. */
    cfg.gap_ms = 0;
    cfg.ramp_ms = 0;
    tone_gen_dtmf_init(&g, &cfg, "12");
    CHECK(render_dtmf(&g, x, sizeof(x) / sizeof(x[0]), 64) == 800, "no gap, no ramp");
}

static void test_dtmf_separators_and_case(void)
{
    tone_gen_dtmf_config_t cfg = TONE_GEN_DTMF_CONFIG_DEFAULT(8000);
    static int16_t a[8000 * 3], b[8000 * 3];
    tone_gen_dtmf_t g;

    tone_gen_dtmf_init(&g, &cfg, "555-12 34");
    size_t na = render_dtmf(&g, a, sizeof(a) / sizeof(a[0]), 240);
    tone_gen_dtmf_init(&g, &cfg, "5551234");
    size_t nb = render_dtmf(&g, b, sizeof(b) / sizeof(b[0]), 240);
    CHECK(na == nb && memcmp(a, b, na * 2) == 0, "separators are skipped, not played");

    tone_gen_dtmf_init(&g, &cfg, "abcd");
    na = render_dtmf(&g, a, sizeof(a) / sizeof(a[0]), 240);
    tone_gen_dtmf_init(&g, &cfg, "ABCD");
    nb = render_dtmf(&g, b, sizeof(b) / sizeof(b[0]), 240);
    CHECK(na == nb && memcmp(a, b, na * 2) == 0, "letters are case-insensitive");
}

static void test_dtmf_block_independence(void)
{
    static const size_t blocks[] = {1, 7, 240, 512};
    tone_gen_dtmf_config_t cfg = TONE_GEN_DTMF_CONFIG_DEFAULT(8000);
    static int16_t ref[8000 * 4], got[8000 * 4];
    tone_gen_dtmf_t g;

    tone_gen_dtmf_init(&g, &cfg, "0123456789*#,ABCD");
    size_t n = render_dtmf(&g, ref, sizeof(ref) / sizeof(ref[0]), 100000);
    for (size_t b = 0; b < 4; b++) {
        tone_gen_dtmf_init(&g, &cfg, "0123456789*#,ABCD");
        size_t m = render_dtmf(&g, got, sizeof(got) / sizeof(got[0]), blocks[b]);
        CHECK(m == n && memcmp(ref, got, n * 2) == 0, "DTMF differs at block size %zu", blocks[b]);
    }
}

static void test_dtmf_current_and_release(void)
{
    tone_gen_dtmf_config_t cfg = TONE_GEN_DTMF_CONFIG_DEFAULT(8000);
    static int16_t x[8000 * 3];
    tone_gen_dtmf_t g;

    tone_gen_dtmf_init(&g, &cfg, "91");
    CHECK(tone_gen_dtmf_current(&g) == '9', "first key is 9");
    tone_gen_dtmf_fill(&g, x, 800);          /* the whole first key */
    CHECK(tone_gen_dtmf_current(&g) == 0, "a gap follows");
    tone_gen_dtmf_fill(&g, x, 800);          /* the gap */
    CHECK(tone_gen_dtmf_current(&g) == '1', "then the second key");

    /* Release inside a key: it ends in its closing ramp, and nothing else is dialled. */
    tone_gen_dtmf_init(&g, &cfg, "91");
    tone_gen_dtmf_fill(&g, x, 300);
    tone_gen_dtmf_release(&g);
    size_t tail = render_dtmf(&g, x, 8000, 50);
    CHECK(tail == 24, "release ends in the 3 ms ramp (%zu samples)", tail);
    CHECK(tone_gen_dtmf_done(&g), "done");
    CHECK(abs(x[tail - 1]) <= 120, "ends near zero, got %d", x[tail - 1]);

    /* Release in a gap ends at once. */
    tone_gen_dtmf_init(&g, &cfg, "91");
    tone_gen_dtmf_fill(&g, x, 900);
    tone_gen_dtmf_release(&g);
    CHECK(tone_gen_dtmf_done(&g) && tone_gen_dtmf_fill(&g, x, 100) == 0, "release in a gap ends at once");
}

static void test_dtmf_invalid(void)
{
    size_t bad = 99;
    CHECK(tone_gen_dtmf_validate("12x4", &bad) == TONE_GEN_ERR_INVALID_DIGIT && bad == 2, "bad index %zu", bad);
    CHECK(tone_gen_dtmf_validate("1234E", &bad) == TONE_GEN_ERR_INVALID_DIGIT && bad == 4, "E is not a key");
    CHECK(tone_gen_dtmf_validate("555-1234, #", &bad) == ESP_OK, "separators, pause and # are valid");
    CHECK(tone_gen_dtmf_validate("", &bad) == ESP_ERR_INVALID_ARG, "empty string");
    CHECK(tone_gen_dtmf_validate("- -", &bad) == ESP_ERR_INVALID_ARG, "only separators");
    CHECK(tone_gen_dtmf_validate(NULL, &bad) == ESP_ERR_INVALID_ARG, "null");

    uint16_t lo, hi;
    CHECK(tone_gen_dtmf_freqs('5', &lo, &hi) && lo == 770 && hi == 1336, "5 is 770+1336");
    CHECK(tone_gen_dtmf_freqs('d', &lo, &hi) && lo == 941 && hi == 1633, "d is 941+1633");
    CHECK(!tone_gen_dtmf_freqs('x', &lo, &hi), "x is not a key");

    tone_gen_dtmf_config_t cfg = TONE_GEN_DTMF_CONFIG_DEFAULT(8000);
    tone_gen_dtmf_t g;
    CHECK(tone_gen_dtmf_init(&g, &cfg, "12x") == TONE_GEN_ERR_INVALID_DIGIT, "init rejects a bad digit");
    cfg.sample_rate_hz = 3000;
    CHECK(tone_gen_dtmf_init(&g, &cfg, "12") == ESP_ERR_INVALID_ARG, "rate too low for 1633 Hz");
    cfg = (tone_gen_dtmf_config_t)TONE_GEN_DTMF_CONFIG_DEFAULT(8000);
    cfg.low_peak = 20000;
    cfg.high_peak = 20000;
    CHECK(tone_gen_dtmf_init(&g, &cfg, "12") == ESP_ERR_INVALID_ARG, "peaks that could clip");
    cfg = (tone_gen_dtmf_config_t)TONE_GEN_DTMF_CONFIG_DEFAULT(8000);
    cfg.tone_ms = 0;
    CHECK(tone_gen_dtmf_init(&g, &cfg, "12") == ESP_ERR_INVALID_ARG, "zero-length keys");
}

int main(void)
{
    test_oscillator_accuracy();
    test_phase_steps();
    test_oscillator_purity();
    test_tone_frequency_and_length();
    test_ramps();
    test_hard_edges_and_zero_peak();
    test_short_signal_clamps_ramp();
    test_block_size_independence();
    test_release_continuous();
    test_release_without_ramp_and_during_ramp();
    test_sweep();
    test_invalid_arguments();
    test_dtmf_every_key();
    test_dtmf_timing_options();
    test_dtmf_separators_and_case();
    test_dtmf_block_independence();
    test_dtmf_current_and_release();
    test_dtmf_invalid();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
