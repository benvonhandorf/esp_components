#pragma once

/*
 * A USB Audio Class 2 microphone: PCM in, an isochronous IN endpoint out.
 *
 * Two layers. The arithmetic -- how many bytes a stream needs per packet, how many
 * the controller can hold, which rates and channel counts therefore fit -- is plain
 * C with no hardware and a host test, because a stream that is one byte over budget
 * does not fail loudly: the host just never sees the device. The device itself
 * (usb_mic_start() and friends) sits on TinyUSB and is only built for chips with a
 * USB OTG controller.
 *
 * WHY A BUDGET AT ALL. An isochronous packet must fit whole in the endpoint's
 * transmit FIFO, and the controller has one block of FIFO RAM shared by the receive
 * FIFO and every transmit FIFO. On the ESP32-S2/S3 that block is 256 words, which
 * after the receive FIFO and endpoint 0 leaves 178 words: 712 bytes per 1 ms packet,
 * below the 1023 bytes the full-speed protocol allows. The limit is the chip's, not
 * the bus's, and it is shared by every channel of the stream.
 *
 * PACKET SIZE. The device's clock sets the rate and the host's frame clock is
 * unrelated, so each packet carries n-1, n or n+1 frames (TinyUSB's audio driver does
 * that selection from its FIFO fill: CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL). The budget is
 * checked against n+1, `floor(rate * interval) + 1`, which is also what the
 * endpoint's wMaxPacketSize advertises.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MIC_ERR_BASE         0x42000
/* The stream needs a larger packet than the controller can hold. */
#define USB_MIC_ERR_OVER_BUDGET  (USB_MIC_ERR_BASE + 1)

#define USB_MIC_MAX_CHANNELS     16

typedef enum {
    USB_MIC_SPEED_FULL,   /* 12 Mbit/s, 1 ms frames */
    USB_MIC_SPEED_HIGH,   /* 480 Mbit/s, 125 us microframes */
} usb_mic_speed_t;

/* What the USB controller offers. Take it from usb_mic_controller() on target; the
 * fields are public so a host test can describe a chip that is not the one running. */
typedef struct {
    usb_mic_speed_t speed;
    unsigned fifo_words;     /* the whole FIFO RAM, in 32-bit words */
    unsigned ep_count;       /* endpoints, both directions, as the receive FIFO is sized by them */
    unsigned ctrl_ep_size;   /* endpoint 0 packet size, bytes */
} usb_mic_controller_t;

typedef struct {
    uint32_t rate_hz;
    uint8_t channels;        /* 1..USB_MIC_MAX_CHANNELS */
    uint8_t bytes;           /* per sample: 2 (16-bit) or 3 (24-bit packed) */
} usb_mic_config_t;

typedef struct {
    size_t needed;           /* bytes in the largest packet the stream can send */
    size_t budget;           /* bytes the controller can hold */
    uint8_t max_channels;    /* most channels that fit at this rate and width; 0 if none */
    uint32_t max_rate_hz;    /* highest rate that fits at this channel count and width; 0 if none */
} usb_mic_check_t;

/* Controllers this component knows. The FIFO figures are TinyUSB's own
 * (dwc2_esp32.h), which is where the allocation is made. */
extern const usb_mic_controller_t USB_MIC_CONTROLLER_ESP32S3;  /* also the S2 */
extern const usb_mic_controller_t USB_MIC_CONTROLLER_ESP32P4;

/* Microseconds between isochronous packets: 1000 full speed, 125 high speed. */
unsigned usb_mic_interval_us(const usb_mic_controller_t *c);

/* Bytes one packet can carry on this controller: the smaller of what the bus allows
 * in one interval and what the FIFO RAM holds after the receive FIFO and endpoint 0. */
size_t usb_mic_budget_bytes(const usb_mic_controller_t *c);

/* Frames in the largest packet at this rate: floor(rate * interval) + 1. */
unsigned usb_mic_max_frames(const usb_mic_controller_t *c, uint32_t rate_hz);

/* Bytes in the largest packet of a stream. 0 for an invalid configuration. */
size_t usb_mic_packet_bytes(const usb_mic_controller_t *c, const usb_mic_config_t *cfg);

/* The most channels that fit at this rate and sample width; 0 if not even one. */
uint8_t usb_mic_max_channels(const usb_mic_controller_t *c, uint32_t rate_hz, uint8_t bytes);

/* The highest rate that fits this many channels at this width; 0 if none does. */
uint32_t usb_mic_max_rate(const usb_mic_controller_t *c, uint8_t channels, uint8_t bytes);

/*
 * The one validation entry point. ESP_ERR_INVALID_ARG for a configuration that is
 * not a stream at all; USB_MIC_ERR_OVER_BUDGET when it does not fit. `out` is filled
 * either way (when not NULL) so the caller can say what would fit.
 */
esp_err_t usb_mic_check(const usb_mic_controller_t *c, const usb_mic_config_t *cfg,
                        usb_mic_check_t *out);

/* ---- The device ---------------------------------------------------------------- */

/*
 * Everything below needs a USB OTG controller (ESP32-S2, S3, P4, S31). On a chip
 * without one, usb_mic_controller() is NULL and usb_mic_start() returns
 * ESP_ERR_NOT_SUPPORTED, so a caller can report the chip rather than lose the
 * command.
 *
 * Starting the device takes the chip's USB PHY. On a chip whose console is
 * USB-Serial-JTAG that ends the console until the next reset: both want the same
 * pins. Say so before calling, and flush what you were about to print.
 *
 * This component installs its own TinyUSB configuration (audio class only), so it
 * cannot share TinyUSB with esp_tinyusb's CDC console or any other device class.
 */

typedef struct {
    usb_mic_config_t stream;   /* rate, channels, sample width: checked by usb_mic_check() */
    uint16_t vid, pid;         /* 0, 0 -> 0x303A, 0x8000 */
    const char *manufacturer;  /* NULL -> "Espressif" */
    const char *product;       /* NULL -> "USB microphone" */
    const char *serial;        /* NULL -> "000001" */
    int task_core;             /* core for the TinyUSB task, -1 for any */
    unsigned task_priority;    /* 0 -> 5 */
} usb_mic_device_config_t;

typedef struct {
    bool started;              /* TinyUSB is running */
    bool mounted;              /* the host has configured the device */
    bool streaming;            /* the host selected the streaming alternate setting */
    uint32_t rate_hz;          /* what the clock source reports to the host */
    uint8_t channels;
    uint8_t bytes;
    size_t packet_bytes;       /* the endpoint's wMaxPacketSize */
    uint64_t bytes_written;    /* accepted by usb_mic_write() while streaming */
    uint64_t bytes_dropped;    /* refused: nobody listening, or the buffer was full */
    uint32_t overruns;         /* writes that found the buffer full */
    size_t buffer_bytes;       /* the host-side buffer's capacity */
    size_t buffered_bytes;     /* what it holds now */
} usb_mic_stats_t;

/* The running chip's controller, or NULL when it has no USB OTG. */
const usb_mic_controller_t *usb_mic_controller(void);

/*
 * Validate and start. `check` (may be NULL) is filled as by usb_mic_check(), so a
 * refusal can say what would have fit. Returns:
 *   ESP_ERR_NOT_SUPPORTED  this chip has no USB OTG
 *   ESP_ERR_INVALID_STATE  already started
 *   ESP_ERR_INVALID_ARG    not a stream at all
 *   USB_MIC_ERR_OVER_BUDGET
 * and otherwise whatever TinyUSB or the PHY returned.
 */
esp_err_t usb_mic_start(const usb_mic_device_config_t *cfg, usb_mic_check_t *check);

/*
 * Queue interleaved PCM, `channels * bytes` per frame, little-endian. Never blocks.
 * Data is dropped, and counted, while no host is streaming; if the buffer is full the
 * oldest is overwritten and an overrun is counted. Returns bytes accepted.
 */
size_t usb_mic_write(const void *pcm, size_t bytes);

/* Free space in the host-side buffer, in bytes: a producer paces itself on this. */
size_t usb_mic_space(void);

bool usb_mic_streaming(void);
void usb_mic_get_stats(usb_mic_stats_t *out);

/* Stop TinyUSB. The PHY stays an OTG one, so the serial console does not return. */
void usb_mic_stop(void);

#ifdef __cplusplus
}
#endif
