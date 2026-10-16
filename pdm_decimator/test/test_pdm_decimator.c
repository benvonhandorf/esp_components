/*
 * Host test for the PDM decimators.
 *
 * The input is a real sigma-delta bitstream (a second-order modulator at
 * 4.8 MHz) of known tones, so what is checked is the thing the filters are for:
 * the level that comes out for a level that went in, per channel, with the
 * response of each filter worked out here independently of its source.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdm_decimator.h"
#include "pdm_decimator_priv.h"

#define FS_PDM   4.8e6
#define FS_PCM   300000.0
#define NOUT     4096           /* analysed output frames */
#define SKIP     256            /* output frames dropped while the filter fills */
#define NFRAMES  (NOUT + SKIP)  /* raw frames: one word per output at ratio 16 */

static int failures;
static int checks;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* A tone on an analysis bin: k whole cycles in NOUT output frames, so a plain
 * DFT at that bin has no leakage. */
static double bin_hz(int k)
{
    return (double)k * FS_PCM / NOUT;
}

/* Second-order sigma-delta modulator, one bit per 4.8 MHz sample. Returns
 * words, MSB first: the first bit generated is the oldest, in the top bit. */
static void modulate(uint16_t *words, size_t nwords, double hz, double amp, double phase)
{
    double i1 = 0, i2 = 0;
    int y = 1;
    for (size_t w = 0; w < nwords; w++) {
        uint16_t word = 0;
        for (int b = 0; b < 16; b++) {
            size_t n = w * 16 + (size_t)b;
            double x = amp * sin(2 * M_PI * hz * (double)n / FS_PDM + phase);
            i1 += x - y;
            i2 += i1 - y;
            y = i2 >= 0 ? 1 : -1;
            word = (uint16_t)((word << 1) | (y > 0 ? 1 : 0));
        }
        words[w] = word;
    }
}

static uint16_t reverse16(uint16_t v)
{
    uint16_t r = 0;
    for (int i = 0; i < 16; i++) {
        r = (uint16_t)((r << 1) | ((v >> i) & 1));
    }
    return r;
}

/* Amplitude of the component at bin k of the analysed window, as a fraction of
 * full scale (32768). */
static double tone_level(const int16_t *out, int channels, int c, int k)
{
    double re = 0, im = 0;
    for (int i = 0; i < NOUT; i++) {
        double v = out[(size_t)(SKIP + i) * (size_t)channels + (size_t)c] / 32768.0;
        double a = 2 * M_PI * (double)k * (double)i / NOUT;
        re += v * cos(a);
        im += v * sin(a);
    }
    return 2.0 * sqrt(re * re + im * im) / NOUT;
}

static double db(double x)
{
    return 20 * log10(x > 1e-12 ? x : 1e-12);
}

/* |H| of a boxcar of length n at frequency f, sample rate fs, as a gain of 1. */
static double box_gain(double f, double fs, int n)
{
    double x = M_PI * f / fs;
    return fabs(sin(x * n) / (n * sin(x)));
}

/* Expected gain of the sinc^6 by 16 at f. */
static double sinc6_gain(double f)
{
    return pow(box_gain(f, FS_PDM, 16), 6);
}

/* Expected gain of the two-stage filter at f: sinc^4 by 8 at 4.8 MHz, then the
 * stage-2 FIR at 600 kHz, normalised by its own DC gain. */
static double twostage_gain(double f)
{
    double re = 0, im = 0, dc = 0;
    for (int k = 0; k < PDM_STAGE2_TAPS; k++) {
        double a = 2 * M_PI * f * k / 600000.0;
        re += pdm_stage2_coef[k] * cos(a);
        im -= pdm_stage2_coef[k] * sin(a);
        dc += pdm_stage2_coef[k];
    }
    return pow(box_gain(f, FS_PDM, 8), 4) * sqrt(re * re + im * im) / dc;
}

static pdm_decimator_t *make(pdm_decimator_filter_t f, int channels, bool lsb)
{
    pdm_decimator_config_t cfg = {
        .filter = f, .channels = channels, .ratio = 16, .lsb_first = lsb, .allow_simd = false,
    };
    pdm_decimator_t *d = NULL;
    esp_err_t err = pdm_decimator_create(&cfg, &d);
    CHECK(err == ESP_OK && d, "create filter %d channels %d: %d", f, channels, err);
    return d;
}

static size_t run(pdm_decimator_t *d, const uint16_t *raw, size_t frames, int16_t *out,
                  size_t chunk)
{
    size_t total = 0;
    int ch = pdm_decimator_channels(d);
    for (size_t at = 0; at < frames; at += chunk) {
        size_t n = frames - at < chunk ? frames - at : chunk;
        size_t got = 0;
        esp_err_t err = pdm_decimator_process(d, raw + at * (size_t)ch, n,
                                              out + total * (size_t)ch, &got);
        CHECK(err == ESP_OK, "process: %d", err);
        CHECK(got <= pdm_decimator_max_output(d, n), "produced %zu > bound %zu", got,
              pdm_decimator_max_output(d, n));
        total += got;
    }
    return total;
}

static void test_dc(void)
{
    static uint16_t ones[1024], zeros[1024];
    static int16_t out[1024];
    memset(ones, 0xFF, sizeof(ones));
    for (int f = 0; f < 2; f++) {
        pdm_decimator_t *d = make(f ? PDM_DECIMATOR_TWO_STAGE : PDM_DECIMATOR_SINC6, 1, false);
        size_t n = run(d, ones, 1024, out, 1024);
        CHECK(n >= 900 && out[n - 1] == 32767, "filter %d: all ones -> %d", f, out[n - 1]);
        pdm_decimator_destroy(d);

        d = make(f ? PDM_DECIMATOR_TWO_STAGE : PDM_DECIMATOR_SINC6, 1, false);
        n = run(d, zeros, 1024, out, 1024);
        CHECK(n >= 900 && out[n - 1] == -32768, "filter %d: all zeros -> %d", f, out[n - 1]);
        pdm_decimator_destroy(d);
    }
}

static void test_response(void)
{
    static uint16_t raw[NFRAMES];
    static int16_t out[NFRAMES];
    /* 10 kHz, 50 kHz and 80 kHz (bins 136, 683, 1092). */
    const int bins[] = {136, 683, 1092};
    for (size_t t = 0; t < sizeof(bins) / sizeof(bins[0]); t++) {
        double hz = bin_hz(bins[t]);
        modulate(raw, NFRAMES, hz, 0.5, 0.3);
        for (int f = 0; f < 2; f++) {
            pdm_decimator_t *d = make(f ? PDM_DECIMATOR_TWO_STAGE : PDM_DECIMATOR_SINC6, 1, false);
            size_t n = run(d, raw, NFRAMES, out, NFRAMES);
            CHECK(n == NFRAMES, "produced %zu of %d", n, NFRAMES);
            double want = 0.5 * (f ? twostage_gain(hz) : sinc6_gain(hz));
            double got = tone_level(out, 1, 0, bins[t]);
            CHECK(fabs(db(got) - db(want)) < 0.5,
                  "filter %d %.0f Hz: got %.2f dBFS, expected %.2f", f, hz, db(got), db(want));
            pdm_decimator_destroy(d);
        }
    }
}

static void test_filters_agree(void)
{
    /* The two-stage filter droops less than the sinc^6, so it reads higher, by
     * up to about 3 dB at 80 kHz (2.9 here). The bound is what notices that
     * response being undone, not a claim that they are the same filter. */
    static uint16_t raw[NFRAMES];
    static int16_t o1[NFRAMES], o2[NFRAMES];
    for (int k = 68; k <= 1092; k += 204) {            /* 5 kHz to 80 kHz */
        modulate(raw, NFRAMES, bin_hz(k), 0.5, 0);
        pdm_decimator_t *a = make(PDM_DECIMATOR_SINC6, 1, false);
        pdm_decimator_t *b = make(PDM_DECIMATOR_TWO_STAGE, 1, false);
        run(a, raw, NFRAMES, o1, NFRAMES);
        run(b, raw, NFRAMES, o2, NFRAMES);
        double diff = db(tone_level(o2, 1, 0, k)) - db(tone_level(o1, 1, 0, k));
        CHECK(diff > -0.5 && diff < 3.2, "%.0f Hz: two-stage is %+.2f dB from sinc6", bin_hz(k), diff);
        pdm_decimator_destroy(a);
        pdm_decimator_destroy(b);
    }
}

static void test_lsb_first(void)
{
    /* The same stream with every word bit-reversed, and the filter told so,
     * must give the same samples. */
    static uint16_t raw[NFRAMES], rev[NFRAMES];
    static int16_t o1[NFRAMES], o2[NFRAMES];
    modulate(raw, NFRAMES, bin_hz(300), 0.5, 1.0);
    for (size_t i = 0; i < NFRAMES; i++) {
        rev[i] = reverse16(raw[i]);
    }
    for (int f = 0; f < 2; f++) {
        pdm_decimator_filter_t kind = f ? PDM_DECIMATOR_TWO_STAGE : PDM_DECIMATOR_SINC6;
        pdm_decimator_t *a = make(kind, 1, false);
        pdm_decimator_t *b = make(kind, 1, true);
        size_t na = run(a, raw, NFRAMES, o1, NFRAMES);
        size_t nb = run(b, rev, NFRAMES, o2, NFRAMES);
        CHECK(na == nb && memcmp(o1, o2, na * sizeof(int16_t)) == 0,
              "filter %d: lsb_first on reversed words differs", f);
        pdm_decimator_destroy(a);
        pdm_decimator_destroy(b);
    }
}

static void test_chunking(void)
{
    /* Four channels, fed in chunks of awkward sizes, give exactly the output of
     * feeding them whole. Phase and history have to survive a call boundary. */
    enum { CH = 4 };
    static uint16_t raw[NFRAMES * CH];
    static int16_t whole[NFRAMES * CH], chunked[NFRAMES * CH];
    static uint16_t one[NFRAMES];
    const int bins[CH] = {136, 273, 409, 546};
    for (int c = 0; c < CH; c++) {
        modulate(one, NFRAMES, bin_hz(bins[c]), 0.4, c);
        for (size_t i = 0; i < NFRAMES; i++) {
            raw[i * CH + (size_t)c] = one[i];
        }
    }
    const size_t chunks[] = {1, 7, 64, 333};
    for (int f = 0; f < 2; f++) {
        pdm_decimator_filter_t kind = f ? PDM_DECIMATOR_TWO_STAGE : PDM_DECIMATOR_SINC6;
        pdm_decimator_t *a = make(kind, CH, false);
        size_t nw = run(a, raw, NFRAMES, whole, NFRAMES);
        pdm_decimator_destroy(a);
        for (size_t t = 0; t < sizeof(chunks) / sizeof(chunks[0]); t++) {
            pdm_decimator_t *b = make(kind, CH, false);
            size_t nc = run(b, raw, NFRAMES, chunked, chunks[t]);
            CHECK(nc == nw && memcmp(whole, chunked, nw * CH * sizeof(int16_t)) == 0,
                  "filter %d chunk %zu differs from one call (%zu vs %zu frames)", f, chunks[t],
                  nc, nw);
            pdm_decimator_destroy(b);
        }
    }
}

static void test_channels_independent(void)
{
    /* Four channels carrying four different tones, as two data lines with a
     * left and a right microphone each would. Each output channel must hold its
     * own tone at the right level and nothing of the other three. */
    enum { CH = 4 };
    static uint16_t raw[NFRAMES * CH];
    static int16_t out[NFRAMES * CH];
    static uint16_t one[NFRAMES];
    const int bins[CH] = {136, 273, 683, 1024};         /* 10, 20, 50, 75 kHz */
    for (int c = 0; c < CH; c++) {
        modulate(one, NFRAMES, bin_hz(bins[c]), 0.5, 0.7 * c);
        for (size_t i = 0; i < NFRAMES; i++) {
            raw[i * CH + (size_t)c] = one[i];
        }
    }
    for (int f = 0; f < 2; f++) {
        pdm_decimator_t *d = make(f ? PDM_DECIMATOR_TWO_STAGE : PDM_DECIMATOR_SINC6, CH, false);
        run(d, raw, NFRAMES, out, NFRAMES);
        for (int c = 0; c < CH; c++) {
            double hz = bin_hz(bins[c]);
            double want = 0.5 * (f ? twostage_gain(hz) : sinc6_gain(hz));
            double own = tone_level(out, CH, c, bins[c]);
            CHECK(fabs(db(own) - db(want)) < 0.5, "filter %d channel %d: %.2f dBFS, expected %.2f",
                  f, c, db(own), db(want));
            for (int o = 0; o < CH; o++) {
                if (o == c) {
                    continue;
                }
                double leak = tone_level(out, CH, c, bins[o]);
                CHECK(db(leak) < db(own) - 50, "filter %d channel %d hears channel %d at %.1f dB",
                      f, c, o, db(leak) - db(own));
            }
        }
        pdm_decimator_destroy(d);
    }
}

static void test_ratios(void)
{
    /* A larger sinc ratio is the same filter at a lower output rate. 32 gives
     * 150 kHz PCM, so a 10 kHz tone must come out at the sinc^6 by 32 gain. */
    static uint16_t raw[NFRAMES * 2];
    static int16_t out[NFRAMES];
    double hz = bin_hz(136);
    modulate(raw, NFRAMES * 2, hz, 0.5, 0);
    pdm_decimator_config_t cfg = {.filter = PDM_DECIMATOR_SINC6, .channels = 1, .ratio = 32};
    pdm_decimator_t *d = NULL;
    CHECK(pdm_decimator_create(&cfg, &d) == ESP_OK, "ratio 32");
    CHECK(pdm_decimator_ratio(d) == 32 && pdm_decimator_input_frames(d, 100) == 200,
          "ratio 32 reports %d, %zu", pdm_decimator_ratio(d), pdm_decimator_input_frames(d, 100));
    size_t n = run(d, raw, NFRAMES * 2, out, NFRAMES * 2);
    CHECK(n == NFRAMES, "ratio 32 produced %zu", n);
    /* 150 kHz output: the analysis window is NOUT / 2 frames, so the tone that
     * was bin 136 of NOUT at 300 kHz is bin 136 of NOUT / 2 at 150 kHz. */
    double re = 0, im = 0;
    const int len = NOUT / 2;
    for (int i = 0; i < len; i++) {
        double v = out[SKIP / 2 + i] / 32768.0;
        double a = 2 * M_PI * 136.0 * i / len;
        re += v * cos(a);
        im += v * sin(a);
    }
    double got = 2 * sqrt(re * re + im * im) / len;
    double want = 0.5 * pow(box_gain(hz, FS_PDM, 32), 6);
    CHECK(fabs(db(got) - db(want)) < 0.5, "ratio 32, 10 kHz: %.2f dBFS, expected %.2f", db(got),
          db(want));
    pdm_decimator_destroy(d);
}

static void test_arguments(void)
{
    pdm_decimator_t *d = (pdm_decimator_t *)1;
    pdm_decimator_config_t c = {.filter = PDM_DECIMATOR_SINC6, .channels = 2, .ratio = 16};

    CHECK(pdm_decimator_create(NULL, &d) == ESP_ERR_INVALID_ARG && d == NULL, "NULL config");
    CHECK(pdm_decimator_create(&c, NULL) == ESP_ERR_INVALID_ARG, "NULL out");

    c.channels = 0;
    CHECK(pdm_decimator_create(&c, &d) == PDM_DECIMATOR_ERR_BAD_CHANNELS, "0 channels");
    c.channels = PDM_DECIMATOR_MAX_CHANNELS + 1;
    CHECK(pdm_decimator_create(&c, &d) == PDM_DECIMATOR_ERR_BAD_CHANNELS, "too many channels");
    c.channels = PDM_DECIMATOR_MAX_CHANNELS;
    CHECK(pdm_decimator_create(&c, &d) == ESP_OK, "the maximum channel count");
    pdm_decimator_destroy(d);

    c.channels = 2;
    const int bad[] = {0, 8, 15, 17, 20, 80};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        c.ratio = bad[i];
        CHECK(pdm_decimator_create(&c, &d) == PDM_DECIMATOR_ERR_BAD_RATIO && d == NULL,
              "sinc6 ratio %d accepted", bad[i]);
    }
    c.filter = PDM_DECIMATOR_TWO_STAGE;
    c.ratio = 32;
    CHECK(pdm_decimator_create(&c, &d) == PDM_DECIMATOR_ERR_BAD_RATIO, "two-stage ratio 32");
    c.ratio = 0;
    CHECK(pdm_decimator_create(&c, &d) == ESP_OK && pdm_decimator_ratio(d) == 16,
          "two-stage ratio 0 means 16");
    CHECK(!pdm_decimator_uses_simd(d), "simd on a host");

    size_t got;
    int16_t out[8];
    uint16_t raw[8] = {0};
    CHECK(pdm_decimator_process(NULL, raw, 4, out, &got) == ESP_ERR_INVALID_ARG, "NULL decimator");
    CHECK(pdm_decimator_process(d, NULL, 4, out, &got) == ESP_ERR_INVALID_ARG, "NULL raw");
    CHECK(pdm_decimator_process(d, raw, 0, out, &got) == ESP_OK && got == 0, "zero frames");
    CHECK(pdm_decimator_reserve(d, 1024) == ESP_OK, "reserve");
    pdm_decimator_destroy(d);
    pdm_decimator_destroy(NULL);
}

int main(void)
{
    test_dc();
    test_response();
    test_filters_agree();
    test_lsb_first();
    test_chunking();
    test_channels_independent();
    test_ratios();
    test_arguments();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
