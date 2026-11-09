#pragma once

/*
 * USB Audio Class 2 descriptors for a microphone, built at run time.
 *
 * Plain bytes, no TinyUSB types, so the host test can walk what comes out. The
 * channel count and sample rate are not compile-time constants here because one
 * firmware serves a two-microphone board at 48 kHz and a one-microphone ultrasonic
 * rig at 192 kHz.
 *
 * Topology: Input Terminal (microphone, N channels) -> Output Terminal (USB
 * streaming), both on one internal fixed Clock Source. There is no Feature Unit: it
 * would only add mute and volume controls that a measurement rig has no use for, and
 * each one is a control request the firmware would have to answer.
 *
 * The rate is carried by the clock source, not by the streaming interface: UAC2
 * has the host ask the clock what it runs at (GET_CUR) and TinyUSB's audio driver
 * sizes its packets from that answer.
 */

#include <stddef.h>
#include <stdint.h>

#include "usb_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MIC_ITF_AC         0   /* audio control interface number */
#define USB_MIC_ITF_AS         1   /* audio streaming interface number */
#define USB_MIC_EP_IN          0x81
#define USB_MIC_ID_IT          1   /* entity ids */
#define USB_MIC_ID_OT          3
#define USB_MIC_ID_CLOCK       4

/* The audio function's descriptors, from the interface association to the last
 * endpoint descriptor. tusb/tusb_config.h repeats this number as
 * CFG_TUD_AUDIO_FUNC_1_DESC_LEN; TinyUSB needs it at compile time. */
#define USB_MIC_FUNC_DESC_LEN   118
#define USB_MIC_CONFIG_DESC_LEN (9 + USB_MIC_FUNC_DESC_LEN)

#define USB_MIC_DEVICE_DESC_LEN 18

/* Writes the 18-byte device descriptor; returns its length (0 if `cap` is short). */
size_t usb_mic_build_device_desc(uint8_t *buf, size_t cap, uint16_t vid, uint16_t pid);

/*
 * Writes the whole configuration descriptor for a validated stream and returns its
 * length; 0 if `cap` is too small or `cfg` is not a stream. `packet_bytes` is the
 * endpoint's wMaxPacketSize, which the caller takes from usb_mic_packet_bytes().
 */
size_t usb_mic_build_config_desc(uint8_t *buf, size_t cap, const usb_mic_config_t *cfg,
                                 uint16_t packet_bytes);

/* Upper bound for a buffer: the descriptor does not grow with the channel count. */
#define USB_MIC_CONFIG_DESC_MAX 128

#ifdef __cplusplus
}
#endif
