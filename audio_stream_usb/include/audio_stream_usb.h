#pragma once

/*
 * A USB Audio Class 2 microphone as an audio_stream sink: the stream's PCM goes to
 * usb_mic_write(), and the host sees a microphone that arecord, Audacity or a browser can
 * record from.
 *
 * Starting the device takes the chip's USB PHY. On a chip whose console is USB-Serial-JTAG
 * that ends the console until the next reset. This component does not warn, silence the
 * log or arm a reset timer -- those are about the application's console, not about USB
 * streaming -- but it gives the application the moment to: `before_usb`.
 *
 * The device stays up when the stream ends by itself (the microphone was closed, say), so
 * the host does not see it vanish and reappear: audio_stream_usb_stop() is what ends it.
 */

#include <stdbool.h>

#include "audio_stream.h"
#include "usb_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    audio_stream_options_t stream;   /* owner NULL -> "usb" */
    /* Optional. Called once the stream's buffers exist and just before USB starts, which
     * is the last moment the console is still there: print the warning, flush it, silence
     * logging, arm a reset timer. Not called when the format cannot be streamed at all. */
    void (*before_usb)(void *ctx);
    void *before_ctx;
    int usb_task_core;               /* core for the TinyUSB task: 0 by default, -1 for any */
} audio_stream_usb_options_t;

/*
 * Whether `fmt` fits the controller's isochronous FIFO budget: ESP_OK, USB_MIC_ERR_OVER_BUDGET,
 * ESP_ERR_INVALID_ARG, or ESP_ERR_NOT_SUPPORTED on a chip with no USB OTG. `out` is filled
 * in every case that has a controller, so a refusal can say what would fit.
 */
esp_err_t audio_stream_usb_check(const audio_stream_fmt_t *fmt, usb_mic_check_t *out);

/*
 * Check, start the USB device and the stream. ESP_ERR_INVALID_STATE if a USB stream is
 * already up (stopped or not), AUDIO_STREAM_ERR_BUSY if something else is streaming the
 * microphone, otherwise the errors of audio_stream_start() and usb_mic_start().
 */
esp_err_t audio_stream_usb_start(const audio_stream_source_t *src, const audio_stream_fmt_t *fmt,
                                 const audio_stream_usb_options_t *opt);

/* End the stream and stop USB. ESP_ERR_INVALID_STATE when none is up. */
esp_err_t audio_stream_usb_stop(void);

/* A USB stream has been started and not stopped, even if it has since ended by itself. */
bool audio_stream_usb_active(void);

#ifdef __cplusplus
}
#endif
