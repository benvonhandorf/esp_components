/*
 * TinyUSB configuration for usb_mic: a device-mode audio class and nothing else.
 *
 * The structure follows espressif/usb_device_uac (esp-iot-solution, Apache-2.0), which
 * injects its configuration into the TinyUSB component the same way. The difference
 * is that nothing here depends on the channel count or the sample rate: the
 * descriptors are built at run time and TinyUSB's audio driver reads both from them,
 * so only buffer *capacities* are fixed.
 */
#ifndef USB_MIC_TUSB_CONFIG_H_
#define USB_MIC_TUSB_CONFIG_H_

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#  define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#  define CFG_TUD_MAX_SPEED           OPT_MODE_HIGH_SPEED
#else
#  define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#  define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#endif

#define CFG_TUSB_OS                   OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH          freertos/
#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif
#define CFG_TUSB_DEBUG                0
#define CFG_TUD_ENABLED               1
#define CFG_TUD_ENDPOINT0_SIZE        64

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN            __attribute__((aligned(4)))
#endif

/* One audio function: a microphone. */
#define CFG_TUD_AUDIO                 1
#define CFG_TUD_AUDIO_ENABLE_EP_IN    1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT   0
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 0
#define CFG_TUD_AUDIO_ENABLE_ENCODING 0
#define CFG_TUD_AUDIO_ENABLE_DECODING 0

/*
 * Async IN flow control: TinyUSB picks each packet's size (n-1, n or n+1 frames) from
 * how full the buffer is relative to half its depth, which is what absorbs the drift
 * between our PCM clock and the host's frame clock. It needs the buffer to be at
 * least four nominal packets deep.
 */
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 1

/* TinyUSB 0.21 measures the function's descriptor length itself (from the interface
 * association to the end of the audio streaming interfaces), so the old
 * CFG_TUD_AUDIO_FUNC_1_DESC_LEN is gone. */
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT        1
/* One control buffer for every function: the largest request is the clock's range
 * reply, 14 bytes. */
#define CFG_TUD_AUDIO_CTRL_BUF_SZ            64

/* Capacities. The largest packet on any supported controller is 1024 bytes (high
 * speed); the real size is whatever the descriptor says, always at or below this.
 * The buffer holds 12 maximum packets: about 17 ms at the full-speed S3 ceiling
 * of 712 bytes per ms. */
#define USB_MIC_EP_MAX_BYTES                 1024
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX    USB_MIC_EP_MAX_BYTES
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (USB_MIC_EP_MAX_BYTES * 12)
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX   16

#ifdef __cplusplus
}
#endif

#endif /* USB_MIC_TUSB_CONFIG_H_ */
