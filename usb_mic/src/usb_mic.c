/*
 * The device half of usb_mic: TinyUSB's audio class driven from a handful of
 * callbacks. The arithmetic and descriptors are in usb_mic_calc.c and usb_mic_desc.c.
 */

#include "usb_mic.h"

#include "soc/soc_caps.h"
#include "sdkconfig.h"

#if SOC_USB_OTG_SUPPORTED

#include <string.h>

#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

#include "usb_mic_desc.h"

/* State is file-static because TinyUSB's callbacks carry no context and there is one
 * USB device per chip. */
static struct {
    bool started;
    bool mounted;
    volatile bool streaming;
    volatile bool stop;
    usb_phy_handle_t phy;
    TaskHandle_t task;
    SemaphoreHandle_t exited;

    usb_mic_config_t stream;
    size_t packet_bytes;
    uint8_t device_desc[USB_MIC_DEVICE_DESC_LEN];
    uint8_t config_desc[USB_MIC_CONFIG_DESC_MAX];
    char manufacturer[32], product[32], serial[32];

    uint64_t bytes_written, bytes_dropped;
    uint32_t overruns;
} s;

/* ---- the chip -------------------------------------------------------------------- */

const usb_mic_controller_t *usb_mic_controller(void)
{
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    return &USB_MIC_CONTROLLER_ESP32P4;
#else
    return &USB_MIC_CONTROLLER_ESP32S3;
#endif
}

/* ---- descriptors TinyUSB asks for ------------------------------------------------- */

uint8_t const *tud_descriptor_device_cb(void)
{
    return s.device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s.config_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[33];
    const char *str = NULL;
    size_t n;

    if (index == 0) {
        buf[1] = 0x0409;      /* English (US) */
        n = 1;
    } else {
        str = index == 1 ? s.manufacturer : index == 2 ? s.product : index == 3 ? s.serial : NULL;
        if (!str) {
            return NULL;
        }
        n = strlen(str);
        if (n > 31) {
            n = 31;
        }
        for (size_t i = 0; i < n; i++) {
            buf[1 + i] = (uint8_t)str[i];
        }
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}

/* ---- device state ----------------------------------------------------------------- */

void tud_mount_cb(void)   { s.mounted = true; }
void tud_umount_cb(void)  { s.mounted = false; s.streaming = false; }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; s.streaming = false; }
void tud_resume_cb(void)  { }

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t itf = tu_u16_low(p_request->wIndex);
    uint8_t alt = tu_u16_low(p_request->wValue);
    if (itf == USB_MIC_ITF_AS) {
        s.streaming = (alt != 0);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    if (tu_u16_low(p_request->wIndex) == USB_MIC_ITF_AS) {
        s.streaming = false;
    }
    return true;
}

/* ---- the clock source -------------------------------------------------------------
 *
 * One rate, so the answers never change. TinyUSB's flow control takes the rate from
 * the reply to GET_CUR of the clock's frequency control (audiod_calc_tx_packet_sz),
 * which is why a host that never SETs it still gets correctly sized packets. */

typedef struct TU_ATTR_PACKED {
    uint16_t num_sub_ranges;
    uint32_t min, max, res;
} clock_range_t;

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t entity = TU_U16_HIGH(req->wIndex);
    uint8_t ctrl = TU_U16_HIGH(req->wValue);

    if (entity != USB_MIC_ID_CLOCK) {
        return false;
    }
    if (ctrl == AUDIO_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            uint32_t rate = s.stream.rate_hz;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &rate, sizeof rate);
        }
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            clock_range_t r = {1, s.stream.rate_hz, s.stream.rate_hz, 0};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &r, sizeof r);
        }
    } else if (ctrl == AUDIO_CS_CTRL_CLK_VALID && req->bRequest == AUDIO_CS_REQ_CUR) {
        uint8_t valid = 1;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, sizeof valid);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    (void)rhport;
    /* The frequency control is read-only in the descriptor; a host that SETs it anyway
     * is accepted only when it asks for the rate we already run. */
    if (TU_U16_HIGH(req->wIndex) == USB_MIC_ID_CLOCK &&
        TU_U16_HIGH(req->wValue) == AUDIO_CS_CTRL_SAM_FREQ &&
        req->bRequest == AUDIO_CS_REQ_CUR) {
        return tu_unaligned_read32(buf) == s.stream.rate_hz;
    }
    return false;
}

/* ---- TinyUSB task ----------------------------------------------------------------- */

static void tusb_task(void *arg)
{
    (void)arg;
    while (!s.stop) {
        tud_task_ext(20, false);
    }
    tusb_deinit(0);
    xSemaphoreGive(s.exited);
    vTaskDelete(NULL);
}

static void copy_str(char *dst, size_t cap, const char *src, const char *fallback)
{
    strlcpy(dst, src ? src : fallback, cap);
}

/* ---- public ----------------------------------------------------------------------- */

esp_err_t usb_mic_start(const usb_mic_device_config_t *cfg, usb_mic_check_t *check)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = usb_mic_check(usb_mic_controller(), &cfg->stream, check);
    if (err != ESP_OK) {
        return err;
    }
    if (s.started) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s.stream, 0, sizeof s.stream);
    s.stream = cfg->stream;
    s.packet_bytes = usb_mic_packet_bytes(usb_mic_controller(), &cfg->stream);
    s.bytes_written = s.bytes_dropped = 0;
    s.overruns = 0;
    s.mounted = s.streaming = s.stop = false;
    copy_str(s.manufacturer, sizeof s.manufacturer, cfg->manufacturer, "Espressif");
    copy_str(s.product, sizeof s.product, cfg->product, "USB microphone");
    copy_str(s.serial, sizeof s.serial, cfg->serial, "000001");

    if (usb_mic_build_device_desc(s.device_desc, sizeof s.device_desc,
                                  cfg->vid ? cfg->vid : 0x303A, cfg->pid ? cfg->pid : 0x8000) == 0 ||
        usb_mic_build_config_desc(s.config_desc, sizeof s.config_desc, &cfg->stream,
                                  (uint16_t)s.packet_bytes) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s.exited) {
        s.exited = xSemaphoreCreateBinary();
        if (!s.exited) {
            return ESP_ERR_NO_MEM;
        }
    }

    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
        .otg_speed = USB_PHY_SPEED_HIGH,
#endif
    };
    err = usb_new_phy(&phy_conf, &s.phy);
    if (err != ESP_OK) {
        return err;
    }
    if (!tusb_init()) {
        usb_del_phy(s.phy);
        s.phy = NULL;
        return ESP_FAIL;
    }

    int core = cfg->task_core;
    BaseType_t ok = xTaskCreatePinnedToCore(tusb_task, "usb_mic", 4096, NULL,
                                            cfg->task_priority ? cfg->task_priority : 5, &s.task,
                                            core < 0 ? tskNO_AFFINITY : core);
    if (ok != pdPASS) {
        tusb_deinit(0);
        usb_del_phy(s.phy);
        s.phy = NULL;
        return ESP_ERR_NO_MEM;
    }
    s.started = true;
    return ESP_OK;
}

void usb_mic_stop(void)
{
    if (!s.started) {
        return;
    }
    s.stop = true;
    xSemaphoreTake(s.exited, portMAX_DELAY);
    if (s.phy) {
        usb_del_phy(s.phy);
        s.phy = NULL;
    }
    s.started = s.mounted = s.streaming = false;
}

bool usb_mic_streaming(void)
{
    return s.started && s.streaming;
}

size_t usb_mic_space(void)
{
    if (!s.started) {
        return 0;
    }
    tu_fifo_t *ff = tud_audio_n_get_ep_in_ff(0);
    uint16_t used = tu_fifo_count(ff);
    return ff->depth > used ? ff->depth - used : 0;
}

size_t usb_mic_write(const void *pcm, size_t bytes)
{
    if (!s.started || !s.streaming) {
        s.bytes_dropped += bytes;
        return 0;
    }
    if (bytes > usb_mic_space()) {
        s.overruns++;     /* the buffer is overwritable: the oldest data goes */
    }
    size_t done = 0;
    while (done < bytes) {
        size_t n = bytes - done > 0xFFFF ? 0xFFFF : bytes - done;
        done += tud_audio_write((const uint8_t *)pcm + done, (uint16_t)n);
    }
    s.bytes_written += done;
    return done;
}

void usb_mic_get_stats(usb_mic_stats_t *out)
{
    *out = (usb_mic_stats_t){0};
    out->started = s.started;
    out->mounted = s.started && s.mounted;
    out->streaming = s.started && s.streaming;
    out->rate_hz = s.stream.rate_hz;
    out->channels = s.stream.channels;
    out->bytes = s.stream.bytes;
    out->packet_bytes = s.packet_bytes;
    out->bytes_written = s.bytes_written;
    out->bytes_dropped = s.bytes_dropped;
    out->overruns = s.overruns;
    if (s.started) {
        tu_fifo_t *ff = tud_audio_n_get_ep_in_ff(0);
        out->buffer_bytes = ff->depth;
        out->buffered_bytes = tu_fifo_count(ff);
    }
}

#else /* !SOC_USB_OTG_SUPPORTED */

const usb_mic_controller_t *usb_mic_controller(void) { return NULL; }

esp_err_t usb_mic_start(const usb_mic_device_config_t *cfg, usb_mic_check_t *check)
{
    (void)cfg;
    if (check) {
        *check = (usb_mic_check_t){0};
    }
    return ESP_ERR_NOT_SUPPORTED;
}

size_t usb_mic_write(const void *pcm, size_t bytes) { (void)pcm; (void)bytes; return 0; }
size_t usb_mic_space(void) { return 0; }
bool usb_mic_streaming(void) { return false; }
void usb_mic_get_stats(usb_mic_stats_t *out) { *out = (usb_mic_stats_t){0}; }
void usb_mic_stop(void) { }

#endif /* SOC_USB_OTG_SUPPORTED */
