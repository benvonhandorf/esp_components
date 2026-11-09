#include <stdio.h>
#include <stdlib.h>

#include "usb_mic.h"
#include "../src/usb_mic_desc.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const usb_mic_controller_t *S3 = &USB_MIC_CONTROLLER_ESP32S3;
static const usb_mic_controller_t *P4 = &USB_MIC_CONTROLLER_ESP32P4;

static esp_err_t chk(const usb_mic_controller_t *c, uint32_t rate, uint8_t ch, uint8_t bytes)
{
    usb_mic_config_t cfg = {.rate_hz = rate, .channels = ch, .bytes = bytes};
    return usb_mic_check(c, &cfg, NULL);
}


/* Walk a configuration descriptor, checking every length, and count what is in it. */
static void check_config(const usb_mic_config_t *cfg, uint16_t packet)
{
    uint8_t buf[USB_MIC_CONFIG_DESC_MAX];
    size_t n = usb_mic_build_config_desc(buf, sizeof buf, cfg, packet);
    CHECK(n == USB_MIC_CONFIG_DESC_LEN);                          /* what TinyUSB is told to walk */
    CHECK(n > 0 && n <= sizeof buf);
    CHECK(buf[2] == (n & 0xFF) && buf[3] == (n >> 8));            /* wTotalLength */
    size_t off = 0;
    unsigned eps = 0, itfs = 0, inputs = 0;
    size_t ac_start = 0, ac_len = 0, ac_seen = 0;
    int in_ac = 0;
    while (off < n) {
        uint8_t len = buf[off], type = buf[off + 1];
        CHECK(len >= 2 && off + len <= n);
        if (type == 0x04) {                                       /* interface */
            itfs++;
            in_ac = (buf[off + 5] == 1 && buf[off + 6] == 1);
        }
        if (type == 0x05) {
            eps++;
            CHECK(buf[off + 2] == 0x81 && buf[off + 3] == 0x05);
            CHECK((buf[off + 4] | (buf[off + 5] << 8)) == packet);
        }
        if (type == 0x24 && buf[off + 2] == 0x01 && in_ac) {      /* AC header */
            ac_start = off;
            ac_len = buf[off + 6] | (buf[off + 7] << 8);
        }
        if (type == 0x24 && in_ac) ac_seen += len;
        if (type == 0x24 && buf[off + 2] == 0x02 && in_ac) {      /* input terminal */
            inputs++;
            CHECK(buf[off + 8] == cfg->channels);
        }
        if (type == 0x24 && buf[off + 2] == 0x01 && !in_ac) {     /* AS general */
            CHECK(buf[off + 10] == cfg->channels);
        }
        if (type == 0x24 && buf[off + 2] == 0x02 && !in_ac) {     /* format type I */
            CHECK(buf[off + 4] == cfg->bytes && buf[off + 5] == cfg->bytes * 8);
        }
        off += len;
    }
    CHECK(off == n);
    CHECK(n - 9 == USB_MIC_FUNC_DESC_LEN);
    CHECK(eps == 1 && itfs == 3 && inputs == 1);
    CHECK(ac_len == ac_seen);                                     /* header's wTotalLength */
    (void)ac_start;
}

int main(void)
{
    /* The S3's budget is the figure the plan was built on: 256 - 62 - 16 = 178 words. */
    CHECK(usb_mic_budget_bytes(S3) == 712);
    CHECK(usb_mic_interval_us(S3) == 1000);

    /* Packet sizes are n+1 frames. */
    CHECK(usb_mic_max_frames(S3, 48000) == 49);
    CHECK(usb_mic_max_frames(S3, 300000) == 301);
    CHECK(usb_mic_max_frames(S3, 44100) == 45);

    /* The table in the plan. */
    CHECK(chk(S3, 300000, 1, 2) == ESP_OK);                      /* 602 */
    CHECK(chk(S3, 300000, 2, 2) == USB_MIC_ERR_OVER_BUDGET);     /* 1204 */
    CHECK(chk(S3, 192000, 1, 2) == ESP_OK);                      /* 386 */
    CHECK(chk(S3, 192000, 2, 2) == USB_MIC_ERR_OVER_BUDGET);     /* 772 */
    CHECK(chk(S3, 150000, 2, 2) == ESP_OK);                      /* 604 */
    CHECK(chk(S3, 150000, 3, 2) == USB_MIC_ERR_OVER_BUDGET);     /* 906 */
    CHECK(chk(S3, 100000, 3, 2) == ESP_OK);                      /* 606 */
    CHECK(chk(S3, 75000, 4, 2) == ESP_OK);                       /* 608 */
    CHECK(chk(S3, 48000, 7, 2) == ESP_OK);                       /* 686 */
    CHECK(chk(S3, 48000, 8, 2) == USB_MIC_ERR_OVER_BUDGET);      /* 784 */
    CHECK(chk(S3, 32000, 8, 2) == ESP_OK);                       /* 528 */
    CHECK(chk(S3, 48000, 2, 2) == ESP_OK);                       /* 196: the Sinn pair */

    /* 24-bit costs 3/2 as much. */
    CHECK(chk(S3, 48000, 4, 3) == ESP_OK);                       /* 588 */
    CHECK(chk(S3, 48000, 5, 3) == USB_MIC_ERR_OVER_BUDGET);      /* 735 */

    /* Max channels and max rate agree with the check, exhaustively. */
    for (uint32_t rate = 8000; rate <= 400000; rate += 1000) {
        for (uint8_t bytes = 2; bytes <= 3; bytes++) {
            uint8_t mc = usb_mic_max_channels(S3, rate, bytes);
            if (mc > 0) CHECK(chk(S3, rate, mc, bytes) == ESP_OK);
            if (mc < USB_MIC_MAX_CHANNELS) CHECK(chk(S3, rate, (uint8_t)(mc + 1), bytes) != ESP_OK);
        }
    }
    for (uint8_t ch = 1; ch <= 8; ch++) {
        uint32_t mr = usb_mic_max_rate(S3, ch, 2);
        CHECK(mr > 0);
        CHECK(chk(S3, mr, ch, 2) == ESP_OK);
        CHECK(chk(S3, mr + 1, ch, 2) != ESP_OK || mr + 1 > mr + 1000);
        /* One kilohertz past the highest rate must be over budget. */
        CHECK(chk(S3, mr + 1000, ch, 2) == USB_MIC_ERR_OVER_BUDGET);
    }
    CHECK(usb_mic_max_rate(S3, 2, 2) == 177999);   /* 178 frames x 4 B = 712 B exactly */
    CHECK(usb_mic_max_rate(S3, 1, 2) == 355999);

    /* Suggestions come back with a refusal. */
    {
        usb_mic_check_t r;
        usb_mic_config_t cfg = {.rate_hz = 300000, .channels = 2, .bytes = 2};
        CHECK(usb_mic_check(S3, &cfg, &r) == USB_MIC_ERR_OVER_BUDGET);
        CHECK(r.needed == 1204 && r.budget == 712);
        CHECK(r.max_channels == 1);
        CHECK(r.max_rate_hz == 177999);
    }

    /* Not a stream at all. */
    CHECK(chk(S3, 0, 2, 2) == ESP_ERR_INVALID_ARG);
    CHECK(chk(S3, 48000, 0, 2) == ESP_ERR_INVALID_ARG);
    CHECK(chk(S3, 48000, 17, 2) == ESP_ERR_INVALID_ARG);
    CHECK(chk(S3, 48000, 2, 4) == ESP_ERR_INVALID_ARG);
    CHECK(usb_mic_check(S3, NULL, NULL) == ESP_ERR_INVALID_ARG);

    /* High speed: 125 us packets, 1024 bytes on the bus, FIFO no longer the limit. */
    CHECK(usb_mic_interval_us(P4) == 125);
    CHECK(usb_mic_budget_bytes(P4) == 1024);
    CHECK(usb_mic_max_frames(P4, 48000) == 7);                   /* 6 + 1 */
    CHECK(chk(P4, 48000, 16, 3) == ESP_OK);                      /* 7*16*3 = 336 */
    CHECK(chk(P4, 192000, 16, 2) == ESP_OK);                     /* 25*16*2 = 800 */
    CHECK(chk(P4, 300000, 8, 2) == ESP_OK);                      /* 38*8*2 = 608 */

    /* Descriptors: every channel count and both widths, at the rates in the plan. */
    for (uint8_t bytes = 2; bytes <= 3; bytes++) {
        for (uint8_t ch = 1; ch <= USB_MIC_MAX_CHANNELS; ch++) {
            usb_mic_config_t cfg = {.rate_hz = 48000, .channels = ch, .bytes = bytes};
            check_config(&cfg, (uint16_t)usb_mic_packet_bytes(P4, &cfg));
        }
    }
    {
        usb_mic_config_t cfg = {.rate_hz = 48000, .channels = 2, .bytes = 2};
        check_config(&cfg, 196);                                  /* the Sinn pair */
        uint8_t small[10], dev[USB_MIC_DEVICE_DESC_LEN];
        CHECK(usb_mic_build_config_desc(small, sizeof small, &cfg, 196) == 0);
        cfg.channels = 0;
        CHECK(usb_mic_build_config_desc(dev, sizeof dev, &cfg, 196) == 0);
        CHECK(usb_mic_build_device_desc(dev, sizeof dev, 0x303A, 0x8000) == 18);
        CHECK(dev[0] == 18 && dev[1] == 1 && dev[4] == 0xEF && dev[8] == 0x3A && dev[9] == 0x30);
        CHECK(usb_mic_build_device_desc(small, sizeof small, 1, 2) == 0);
    }

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("usb_mic: all checks passed\n");
    return 0;
}
