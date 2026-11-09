/*
 * The arithmetic behind usb_mic.h. No hardware, no heap: everything here is checked
 * by test/test_usb_mic.c on a host.
 */

#include "usb_mic.h"

/* The full-speed protocol's isochronous maximum, and the high-speed one per
 * microframe (one transaction per microframe; high-bandwidth endpoints can do more,
 * and are not used here). */
#define FS_ISO_MAX_BYTES 1023
#define HS_ISO_MAX_BYTES 1024

/*
 * TinyUSB's dwc2 driver (dcd_dwc2.c) sizes the FIFO RAM as follows, and the
 * allocations below are copied from it so the budget is what it will actually grant:
 *   receive FIFO  13 + 1 + 2*(largest OUT packet/4 + 1) + 2*ep_count   words
 *   endpoint 0 IN  ctrl_ep_size/4                                      words
 * with no OUT endpoint but 0 in use, the largest OUT packet is endpoint 0's.
 */
const usb_mic_controller_t USB_MIC_CONTROLLER_ESP32S3 = {
    .speed = USB_MIC_SPEED_FULL, .fifo_words = 256, .ep_count = 7, .ctrl_ep_size = 64,
};
const usb_mic_controller_t USB_MIC_CONTROLLER_ESP32P4 = {
    .speed = USB_MIC_SPEED_HIGH, .fifo_words = 1024, .ep_count = 16, .ctrl_ep_size = 64,
};

unsigned usb_mic_interval_us(const usb_mic_controller_t *c)
{
    return c->speed == USB_MIC_SPEED_HIGH ? 125 : 1000;
}

size_t usb_mic_budget_bytes(const usb_mic_controller_t *c)
{
    unsigned rx = 13 + 1 + 2 * (c->ctrl_ep_size / 4 + 1) + 2 * c->ep_count;
    unsigned ep0 = c->ctrl_ep_size / 4;
    unsigned tx = (c->fifo_words > rx + ep0) ? c->fifo_words - rx - ep0 : 0;
    size_t fifo = (size_t)tx * 4;
    size_t bus = c->speed == USB_MIC_SPEED_HIGH ? HS_ISO_MAX_BYTES : FS_ISO_MAX_BYTES;
    return fifo < bus ? fifo : bus;
}

unsigned usb_mic_max_frames(const usb_mic_controller_t *c, uint32_t rate_hz)
{
    return (unsigned)((uint64_t)rate_hz * usb_mic_interval_us(c) / 1000000u) + 1;
}

static bool valid(const usb_mic_config_t *cfg)
{
    return cfg && cfg->rate_hz > 0 && cfg->channels >= 1 &&
           cfg->channels <= USB_MIC_MAX_CHANNELS &&
           (cfg->bytes == 2 || cfg->bytes == 3);
}

size_t usb_mic_packet_bytes(const usb_mic_controller_t *c, const usb_mic_config_t *cfg)
{
    if (!valid(cfg)) {
        return 0;
    }
    return (size_t)usb_mic_max_frames(c, cfg->rate_hz) * cfg->channels * cfg->bytes;
}

uint8_t usb_mic_max_channels(const usb_mic_controller_t *c, uint32_t rate_hz, uint8_t bytes)
{
    if (rate_hz == 0 || (bytes != 2 && bytes != 3)) {
        return 0;
    }
    size_t per_channel = (size_t)usb_mic_max_frames(c, rate_hz) * bytes;
    size_t n = usb_mic_budget_bytes(c) / per_channel;
    return n > USB_MIC_MAX_CHANNELS ? USB_MIC_MAX_CHANNELS : (uint8_t)n;
}

uint32_t usb_mic_max_rate(const usb_mic_controller_t *c, uint8_t channels, uint8_t bytes)
{
    if (channels < 1 || channels > USB_MIC_MAX_CHANNELS || (bytes != 2 && bytes != 3)) {
        return 0;
    }
    /* frames * channels * bytes <= budget, frames = floor(rate*interval/1e6) + 1 */
    size_t frames = usb_mic_budget_bytes(c) / ((size_t)channels * bytes);
    if (frames < 1) {
        return 0;
    }
    /* Largest rate with floor(rate * interval / 1e6) <= frames - 1. */
    uint64_t interval = usb_mic_interval_us(c);
    uint64_t rate = ((uint64_t)frames * 1000000u - 1) / interval;
    return (uint32_t)rate;
}

esp_err_t usb_mic_check(const usb_mic_controller_t *c, const usb_mic_config_t *cfg,
                        usb_mic_check_t *out)
{
    if (!c || !valid(cfg)) {
        if (out) {
            *out = (usb_mic_check_t){0};
        }
        return ESP_ERR_INVALID_ARG;
    }
    usb_mic_check_t r = {
        .needed = usb_mic_packet_bytes(c, cfg),
        .budget = usb_mic_budget_bytes(c),
        .max_channels = usb_mic_max_channels(c, cfg->rate_hz, cfg->bytes),
        .max_rate_hz = usb_mic_max_rate(c, cfg->channels, cfg->bytes),
    };
    if (out) {
        *out = r;
    }
    return r.needed <= r.budget ? ESP_OK : USB_MIC_ERR_OVER_BUDGET;
}
