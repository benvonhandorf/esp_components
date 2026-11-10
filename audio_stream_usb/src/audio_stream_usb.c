#include "audio_stream_usb.h"

#include <string.h>

static struct {
    bool up;
    audio_stream_usb_options_t opt;
} u;

static usb_mic_config_t mic_config(const audio_stream_fmt_t *fmt)
{
    usb_mic_config_t mic = {
        .rate_hz = fmt->rate_hz,
        .channels = fmt->channels,
        .bytes = fmt->bytes,
    };
    return mic;
}

esp_err_t audio_stream_usb_check(const audio_stream_fmt_t *fmt, usb_mic_check_t *out)
{
    const usb_mic_controller_t *ctrl = usb_mic_controller();
    if (!ctrl) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    usb_mic_config_t mic = mic_config(fmt);
    return usb_mic_check(ctrl, &mic, out);
}

static esp_err_t sink_begin(void *ctx, const audio_stream_fmt_t *fmt)
{
    (void)ctx;
    if (u.opt.before_usb) {
        u.opt.before_usb(u.opt.before_ctx);
    }
    usb_mic_device_config_t dev = {
        .stream = mic_config(fmt),
        .task_core = u.opt.usb_task_core,
    };
    esp_err_t err = usb_mic_start(&dev, NULL);
    if (err == ESP_OK) {
        u.up = true;
    }
    return err;
}

static bool sink_write(void *ctx, const uint8_t *pcm, size_t len)
{
    (void)ctx;
    usb_mic_write(pcm, len);     /* drops, and counts, while no host is listening */
    return true;
}

static const audio_stream_sink_t sink = {
    .begin = sink_begin,
    .write = sink_write,
    .end = NULL,
    .release_on_end = false,     /* the device is still up: stop() ends it */
};

esp_err_t audio_stream_usb_start(const audio_stream_source_t *src, const audio_stream_fmt_t *fmt,
                                 const audio_stream_usb_options_t *opt)
{
    if (u.up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!src || !fmt) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = audio_stream_usb_check(fmt, NULL);
    if (err != ESP_OK) {
        return err;
    }

    memset(&u.opt, 0, sizeof u.opt);
    if (opt) {
        u.opt = *opt;
    }
    if (!u.opt.stream.owner) {
        u.opt.stream.owner = "usb";
    }

    err = audio_stream_start(src, fmt, &sink, NULL, &u.opt.stream);
    if (err != ESP_OK && u.up) {
        /* The device came up and the stream task did not: do not leave it with no producer. */
        usb_mic_stop();
        u.up = false;
    }
    return err;
}

esp_err_t audio_stream_usb_stop(void)
{
    if (!u.up) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_stream_stop();         /* INVALID_STATE here only if the stream never started */
    usb_mic_stop();
    u.up = false;
    return ESP_OK;
}

bool audio_stream_usb_active(void)
{
    return u.up;
}
