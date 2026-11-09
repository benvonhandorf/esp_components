#include "usb_mic_desc.h"

#include <string.h>

/* Descriptor type and subtype codes (USB 2.0 and USB Audio Class 2.0). */
#define DT_DEVICE        0x01
#define DT_CONFIG        0x02
#define DT_INTERFACE     0x04
#define DT_ENDPOINT      0x05
#define DT_IAD           0x0B
#define DT_CS_INTERFACE  0x24
#define DT_CS_ENDPOINT   0x25

#define AC_HEADER        0x01
#define AC_INPUT_TERM    0x02
#define AC_OUTPUT_TERM   0x03
#define AC_CLOCK_SOURCE  0x0A
#define AS_GENERAL       0x01
#define AS_FORMAT_TYPE   0x02

#define CLASS_AUDIO      0x01
#define SUBCLASS_AC      0x01
#define SUBCLASS_AS      0x02
#define PROTOCOL_UAC2    0x20

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t len;
    bool overflow;
} wr_t;

static void put(wr_t *w, const void *data, size_t n)
{
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->len, data, n);
    w->len += n;
}

#define PUT(w, ...) do { const uint8_t b_[] = {__VA_ARGS__}; put((w), b_, sizeof b_); } while (0)
#define LE16(v) (uint8_t)((v) & 0xFF), (uint8_t)(((v) >> 8) & 0xFF)

size_t usb_mic_build_device_desc(uint8_t *buf, size_t cap, uint16_t vid, uint16_t pid)
{
    wr_t w = {buf, cap, 0, false};
    PUT(&w, USB_MIC_DEVICE_DESC_LEN, DT_DEVICE,
        LE16(0x0200),             /* bcdUSB */
        0xEF, 0x02, 0x01,         /* miscellaneous / common class / interface association */
        64,                       /* bMaxPacketSize0 */
        LE16(vid), LE16(pid),
        LE16(0x0100),             /* bcdDevice */
        1, 2, 3,                  /* manufacturer, product, serial string indices */
        1);                       /* one configuration */
    return w.overflow ? 0 : w.len;
}

size_t usb_mic_build_config_desc(uint8_t *buf, size_t cap, const usb_mic_config_t *cfg,
                                 uint16_t packet_bytes)
{
    if (!cfg || cfg->channels < 1 || cfg->channels > USB_MIC_MAX_CHANNELS ||
        (cfg->bytes != 2 && cfg->bytes != 3) || packet_bytes == 0) {
        return 0;
    }
    wr_t w = {buf, cap, 0, false};

    /* Entity sizes, so the two total-length fields can be written first. */
    const uint16_t ac_total = 9 + 8 + 17 + 12;   /* header + clock + input + output */
    const uint16_t total = USB_MIC_CONFIG_DESC_LEN;

    /* Configuration: two interfaces, bus powered, 100 mA. */
    PUT(&w, 9, DT_CONFIG, LE16(total), 2, 1, 0, 0x80, 50);

    /* Interface association: the audio function is interfaces 0 and 1. */
    PUT(&w, 8, DT_IAD, USB_MIC_ITF_AC, 2, CLASS_AUDIO, 0x00, PROTOCOL_UAC2, 0);

    /* Audio control interface. */
    PUT(&w, 9, DT_INTERFACE, USB_MIC_ITF_AC, 0, 0, CLASS_AUDIO, SUBCLASS_AC, PROTOCOL_UAC2, 0);
    PUT(&w, 9, DT_CS_INTERFACE, AC_HEADER, LE16(0x0200),
        0x03,                     /* bCategory: microphone */
        LE16(ac_total), 0x00);    /* bmControls */

    /* Clock source: internal, fixed; frequency and validity are read-only. */
    PUT(&w, 8, DT_CS_INTERFACE, AC_CLOCK_SOURCE, USB_MIC_ID_CLOCK,
        0x01, 0x05, 0, 0);

    /* Input terminal: a microphone with `channels` channels. No spatial positions
     * are claimed: the channels are whatever the board wired, not left and right. */
    PUT(&w, 17, DT_CS_INTERFACE, AC_INPUT_TERM, USB_MIC_ID_IT,
        LE16(0x0201),             /* microphone */
        0, USB_MIC_ID_CLOCK, cfg->channels,
        0, 0, 0, 0,               /* bmChannelConfig */
        0, 0, 0, 0);              /* iChannelNames, bmControls, iTerminal */

    /* Output terminal: USB streaming, fed by the input terminal. */
    PUT(&w, 12, DT_CS_INTERFACE, AC_OUTPUT_TERM, USB_MIC_ID_OT,
        LE16(0x0101), 0, USB_MIC_ID_IT, USB_MIC_ID_CLOCK, 0, 0, 0);

    /* Audio streaming interface: alternate 0 idle, alternate 1 streaming. */
    PUT(&w, 9, DT_INTERFACE, USB_MIC_ITF_AS, 0, 0, CLASS_AUDIO, SUBCLASS_AS, PROTOCOL_UAC2, 0);
    PUT(&w, 9, DT_INTERFACE, USB_MIC_ITF_AS, 1, 1, CLASS_AUDIO, SUBCLASS_AS, PROTOCOL_UAC2, 0);
    PUT(&w, 16, DT_CS_INTERFACE, AS_GENERAL, USB_MIC_ID_OT,
        0x00,                     /* bmControls */
        0x01,                     /* bFormatType: type I */
        0x01, 0, 0, 0,            /* bmFormats: PCM */
        cfg->channels,
        0, 0, 0, 0,               /* bmChannelConfig */
        0);                       /* iChannelNames */
    PUT(&w, 6, DT_CS_INTERFACE, AS_FORMAT_TYPE, 0x01,
        cfg->bytes, (uint8_t)(cfg->bytes * 8));

    /* Isochronous IN, asynchronous data endpoint. bInterval 1 is every frame at full
     * speed and every microframe at high speed. */
    PUT(&w, 7, DT_ENDPOINT, USB_MIC_EP_IN, 0x05, LE16(packet_bytes), 1);
    PUT(&w, 8, DT_CS_ENDPOINT, 0x01, 0x00, 0x00, 0x00, 0, 0);

    if (w.overflow || w.len != total) {
        return 0;
    }
    return w.len;
}
