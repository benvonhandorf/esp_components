#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "audio_stream.h"
#include "audio_stream_ring.h"

static int failures;
static int checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

static esp_err_t dummy_read(void *ctx, void *buf, size_t len, size_t *got, uint32_t t)
{
    (void)ctx; (void)buf; (void)len; (void)t;
    *got = 0;
    return ESP_OK;
}

static audio_stream_source_t pcm_source(uint32_t rate, int channels, int slot, int bits)
{
    audio_stream_source_t s = {
        .rate_hz = rate, .channels = (uint8_t)channels, .slot_bytes = (uint8_t)slot,
        .data_bits = (uint8_t)bits, .read = dummy_read,
    };
    return s;
}

static audio_stream_source_t raw_source(uint32_t clk, int channels)
{
    audio_stream_source_t s = {
        .channels = (uint8_t)channels, .slot_bytes = 2, .data_bits = 16,
        .pdm_raw = true, .pdm_clk_hz = clk, .read = dummy_read,
    };
    return s;
}

/* ---- channel list ---------------------------------------------------------------------- */

static void test_parse_channels(void)
{
    uint8_t out[AUDIO_STREAM_MAX_CHANNELS];
    audio_stream_why_t why;
    int n = 0;

    CHECK(audio_stream_parse_channels("0,1,3", 4, out, &n, &why) == ESP_OK);
    CHECK(n == 3 && out[0] == 0 && out[1] == 1 && out[2] == 3);
    CHECK(audio_stream_parse_channels("2", 4, out, &n, &why) == ESP_OK && n == 1 && out[0] == 2);

    CHECK(audio_stream_parse_channels("1,x", 4, out, &n, &why) == AUDIO_STREAM_ERR_BAD_LIST);
    CHECK(audio_stream_parse_channels("0,1,", 4, out, &n, &why) == AUDIO_STREAM_ERR_BAD_LIST);
    CHECK(audio_stream_parse_channels("", 4, out, &n, &why) == AUDIO_STREAM_ERR_BAD_LIST);
    CHECK(audio_stream_parse_channels(",1", 4, out, &n, &why) == AUDIO_STREAM_ERR_BAD_LIST);
    CHECK(audio_stream_parse_channels("-1", 4, out, &n, &why) == AUDIO_STREAM_ERR_BAD_LIST);

    CHECK(audio_stream_parse_channels("0,4", 4, out, &n, &why) == AUDIO_STREAM_ERR_BAD_CHANNEL);
    CHECK(why.channel == 4 && why.channels_available == 4);
    CHECK(audio_stream_parse_channels("1,1", 4, out, &n, &why) == AUDIO_STREAM_ERR_DUP_CHANNEL);
    CHECK(why.channel == 1);

    /* More channels than a stream can carry. */
    CHECK(audio_stream_parse_channels("0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", 32, out, &n, &why) ==
          AUDIO_STREAM_ERR_BAD_LIST);
    CHECK(audio_stream_parse_channels("0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15", 32, out, &n, &why) ==
          ESP_OK && n == 16);
}

/* ---- resolve --------------------------------------------------------------------------- */

static void test_resolve_pcm(void)
{
    audio_stream_source_t src = pcm_source(48000, 2, 4, 24);
    audio_stream_want_t want = {0};
    audio_stream_fmt_t fmt;
    audio_stream_why_t why;

    /* Nothing asked: follow the source, 16 bits. */
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == ESP_OK);
    CHECK(fmt.rate_hz == 48000 && fmt.channels == 2 && fmt.bytes == 2 && fmt.ratio == 0);
    CHECK(fmt.chan[0] == 0 && fmt.chan[1] == 1);

    /* A channel subset, reordered, at 24 bits. */
    want.nch = 2; want.chan[0] = 1; want.chan[1] = 0; want.bits = 24;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == ESP_OK);
    CHECK(fmt.channels == 2 && fmt.bytes == 3 && fmt.chan[0] == 1 && fmt.chan[1] == 0);

    /* The rate is the source's: another one is refused, with the source's in the facts. */
    memset(&want, 0, sizeof want);
    want.rate = 44100;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == AUDIO_STREAM_ERR_BAD_RATE);
    CHECK(why.src_rate == 48000 && why.want_rate == 44100 && why.code == AUDIO_STREAM_ERR_BAD_RATE);
    want.rate = 48000;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == ESP_OK);

    /* A channel the microphone does not have, as when it was reopened with fewer. */
    memset(&want, 0, sizeof want);
    want.nch = 1; want.chan[0] = 2;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == AUDIO_STREAM_ERR_BAD_CHANNEL);
    CHECK(why.channel == 2 && why.channels_available == 2);

    /* 24 bits needs 24 data bits. */
    audio_stream_source_t narrow = pcm_source(16000, 1, 2, 16);
    memset(&want, 0, sizeof want);
    want.bits = 24;
    CHECK(audio_stream_resolve(&narrow, &want, &fmt, &why) == AUDIO_STREAM_ERR_BAD_BITS);
    CHECK(why.src_bits == 16);
    want.bits = 20;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == AUDIO_STREAM_ERR_BAD_BITS);

    /* Not a stream. */
    audio_stream_source_t bad = pcm_source(0, 2, 4, 24);
    CHECK(audio_stream_resolve(&bad, &want, &fmt, &why) == AUDIO_STREAM_ERR_NO_SOURCE);
    bad = pcm_source(48000, 2, 3, 24);
    CHECK(audio_stream_resolve(&bad, &want, &fmt, &why) == AUDIO_STREAM_ERR_NO_SOURCE);
    bad = pcm_source(48000, 0, 4, 24);
    CHECK(!audio_stream_source_valid(&bad));
}

static void test_resolve_raw(void)
{
    audio_stream_source_t src = raw_source(3072000, 2);
    audio_stream_want_t want = {0};
    audio_stream_fmt_t fmt;
    audio_stream_why_t why;

    /* No rate asked for: standard oversampling, so 3.072 MHz is 48 kHz. */
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == ESP_OK);
    CHECK(fmt.ratio == 64 && fmt.rate_hz == 48000);

    want.rate = 64000;                        /* 3.072 MHz / 48 */
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == ESP_OK && fmt.ratio == 48);
    want.rate = 96000;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == ESP_OK && fmt.ratio == 32);
    want.rate = 192000;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == ESP_OK && fmt.ratio == 16);

    want.rate = 44100;
    CHECK(audio_stream_resolve(&src, &want, &fmt, &why) == AUDIO_STREAM_ERR_BAD_RATE);
    CHECK(why.raw_rates[0] == 48000 && why.raw_rates[1] == 64000 &&
          why.raw_rates[2] == 96000 && why.raw_rates[3] == 192000);

    /* A clock that does not divide evenly offers no rate that is not an approximation. */
    audio_stream_source_t odd = raw_source(3072001, 2);
    want.rate = 48000;
    CHECK(audio_stream_resolve(&odd, &want, &fmt, &why) == AUDIO_STREAM_ERR_BAD_RATE);

    audio_stream_source_t nine = raw_source(3072000, 9);
    CHECK(!audio_stream_source_valid(&nine));
}

/* ---- conversion ------------------------------------------------------------------------ */

static void test_convert(void)
{
    audio_stream_fmt_t fmt = {.rate_hz = 48000, .channels = 2, .bytes = 2, .chan = {1, 0}};

    /* 16-bit slots, three channels in, channels 1 and 0 out. */
    const int16_t in16[2 * 3] = {0x0102, 0x0304, 0x0506, 0x0708, 0x090A, 0x0B0C};
    uint8_t out[64];
    size_t n = audio_stream_convert(&fmt, 2, 3, in16, 2, out);
    CHECK(n == 8);
    const int16_t want16[4] = {0x0304, 0x0102, 0x090A, 0x0708};
    CHECK(memcmp(out, want16, 8) == 0);

    /* 16-bit to 16-bit, every channel in order: the one-copy path, same bytes. */
    {
        audio_stream_fmt_t id = {.rate_hz = 48000, .channels = 2, .bytes = 2, .chan = {0, 1}};
        const int16_t two[4] = {0x0102, 0x0304, 0x0506, 0x0708};
        n = audio_stream_convert(&id, 2, 2, two, 2, out);
        CHECK(n == 8 && memcmp(out, two, 8) == 0);
        /* One channel of two. */
        id.channels = 1; id.chan[0] = 1;
        n = audio_stream_convert(&id, 2, 2, two, 2, out);
        const int16_t right[2] = {0x0304, 0x0708};
        CHECK(n == 4 && memcmp(out, right, 4) == 0);
    }

    /* 32-bit slots to 16 bits: the top half. */
    const int32_t in32[2] = {(int32_t)0x12345600, (int32_t)0xFEDCBA00};
    fmt.channels = 1; fmt.chan[0] = 1;
    n = audio_stream_convert(&fmt, 4, 2, in32, 1, out);
    CHECK(n == 2 && out[0] == 0xDC && out[1] == 0xFE);

    /* To 24 bits: the top three bytes, little-endian. */
    fmt.bytes = 3; fmt.chan[0] = 0;
    n = audio_stream_convert(&fmt, 4, 2, in32, 1, out);
    CHECK(n == 3 && out[0] == 0x56 && out[1] == 0x34 && out[2] == 0x12);

    /* Negative values keep their sign. */
    const int32_t neg[1] = {(int32_t)0x80000000};
    fmt.chan[0] = 0;
    n = audio_stream_convert(&fmt, 4, 1, neg, 1, out);
    CHECK(n == 3 && out[0] == 0 && out[1] == 0 && out[2] == 0x80);

    /* 16-bit to 24-bit is left-justified. */
    const int16_t one[1] = {(int16_t)0xABCD};
    n = audio_stream_convert(&fmt, 2, 1, one, 1, out);
    CHECK(n == 3 && out[0] == 0 && out[1] == 0xCD && out[2] == 0xAB);
}

/* ---- WAV header ------------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void test_wav(void)
{
    uint8_t h[AUDIO_STREAM_WAV_HEADER_MAX];

    audio_stream_fmt_t stereo = {.rate_hz = 48000, .channels = 2, .bytes = 2};
    size_t n = audio_stream_wav_header(&stereo, h);
    CHECK(n == 44);
    CHECK(memcmp(h, "RIFF", 4) == 0 && rd32(h + 4) == 0xFFFFFFFFu && memcmp(h + 8, "WAVE", 4) == 0);
    CHECK(memcmp(h + 12, "fmt ", 4) == 0 && rd32(h + 16) == 16 && rd16(h + 20) == 1);
    CHECK(rd16(h + 22) == 2 && rd32(h + 24) == 48000 && rd32(h + 28) == 192000);
    CHECK(rd16(h + 32) == 4 && rd16(h + 34) == 16);
    CHECK(memcmp(h + 36, "data", 4) == 0 && rd32(h + 40) == 0xFFFFFFFFu);

    audio_stream_fmt_t four = {.rate_hz = 96000, .channels = 4, .bytes = 3};
    n = audio_stream_wav_header(&four, h);
    CHECK(n == 68 && n <= AUDIO_STREAM_WAV_HEADER_MAX);
    CHECK(rd32(h + 16) == 40 && rd16(h + 20) == 0xFFFE && rd16(h + 22) == 4);
    CHECK(rd32(h + 24) == 96000 && rd32(h + 28) == 96000u * 12 && rd16(h + 32) == 12);
    CHECK(rd16(h + 34) == 24 && rd16(h + 36) == 22 && rd16(h + 38) == 24 && rd32(h + 40) == 0);
    CHECK(h[44] == 1 && h[45] == 0 && h[58] == 0x9B && h[59] == 0x71);   /* the PCM GUID */
    CHECK(memcmp(h + 60, "data", 4) == 0 && rd32(h + 64) == 0xFFFFFFFFu);

    /* Stereo at 24 bits needs the extensible form too. */
    audio_stream_fmt_t st24 = {.rate_hz = 48000, .channels = 2, .bytes = 3};
    n = audio_stream_wav_header(&st24, h);
    CHECK(n == 68 && rd32(h + 40) == 3);
}

/* ---- ring ------------------------------------------------------------------------------ */

static void test_ring_basic(void)
{
    uint8_t store[9];                         /* capacity 8 */
    audio_stream_ring_t r;
    audio_stream_ring_init(&r, store, sizeof store);
    CHECK(audio_stream_ring_capacity(&r) == 8 && audio_stream_ring_fill(&r) == 0);

    uint8_t a[6] = {1, 2, 3, 4, 5, 6}, out[16];
    CHECK(audio_stream_ring_write(&r, a, 6));
    CHECK(audio_stream_ring_fill(&r) == 6);
    CHECK(!audio_stream_ring_write(&r, a, 3));       /* all or nothing */
    CHECK(audio_stream_ring_fill(&r) == 6);
    CHECK(audio_stream_ring_read(&r, out, 4) == 4 && out[0] == 1 && out[3] == 4);

    /* Wraps around the end of the storage. */
    uint8_t b[5] = {7, 8, 9, 10, 11};
    CHECK(audio_stream_ring_write(&r, b, 5));
    CHECK(audio_stream_ring_fill(&r) == 7);
    CHECK(audio_stream_ring_read(&r, out, 16) == 7);
    const uint8_t want[7] = {5, 6, 7, 8, 9, 10, 11};
    CHECK(memcmp(out, want, 7) == 0);
    CHECK(audio_stream_ring_read(&r, out, 16) == 0);

    uint8_t full[8] = {0};
    CHECK(audio_stream_ring_write(&r, full, 8));      /* exactly the capacity */
    CHECK(!audio_stream_ring_write(&r, full, 1));
    uint8_t nine[9] = {0};
    audio_stream_ring_read(&r, out, 16);
    CHECK(!audio_stream_ring_write(&r, nine, 9));     /* more than it can ever hold */
}

#define STRESS_BYTES (4u * 1000 * 1000)

static audio_stream_ring_t stress_ring;
static uint8_t stress_store[1000 + 1];
static volatile int stress_bad;

static void *stress_producer(void *arg)
{
    (void)arg;
    uint8_t block[96];
    size_t sent = 0;
    uint32_t seq = 0;
    while (sent < STRESS_BYTES) {
        for (size_t i = 0; i < sizeof block; i++) {
            block[i] = (uint8_t)(seq + i);
        }
        if (audio_stream_ring_write(&stress_ring, block, sizeof block)) {
            seq += (uint32_t)sizeof block;
            sent += sizeof block;
        }
    }
    return NULL;
}

static void *stress_consumer(void *arg)
{
    (void)arg;
    uint8_t buf[200];
    uint32_t seq = 0;
    size_t got = 0;
    while (got < (STRESS_BYTES / 96) * 96 + (STRESS_BYTES % 96 ? 96 : 0)) {
        size_t n = audio_stream_ring_read(&stress_ring, buf, sizeof buf);
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != (uint8_t)(seq + i)) {
                stress_bad = 1;
            }
        }
        seq += (uint32_t)n;
        got += n;
    }
    return NULL;
}

/* Every byte arrives, in order, with a producer and a consumer racing on a small ring. */
static void test_ring_threads(void)
{
    audio_stream_ring_init(&stress_ring, stress_store, sizeof stress_store);
    pthread_t p, c;
    pthread_create(&c, NULL, stress_consumer, NULL);
    pthread_create(&p, NULL, stress_producer, NULL);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    CHECK(!stress_bad);
    CHECK(audio_stream_ring_fill(&stress_ring) == 0);
}

int main(void)
{
    test_parse_channels();
    test_resolve_pcm();
    test_resolve_raw();
    test_convert();
    test_wav();
    test_ring_basic();
    test_ring_threads();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
