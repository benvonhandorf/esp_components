# audio_stream_usb

The USB sink for [`audio_stream`](../audio_stream): the stream's PCM goes to
[`usb_mic`](../usb_mic), and the host sees a USB microphone (`arecord`, Audacity, a
browser). ESP32-S2, S3, P4 and S31; on other chips every call reports
`ESP_ERR_NOT_SUPPORTED`.

```c
audio_stream_usb_options_t opt = {
    .stream = {.owner = "usb", .core = 1},
    .before_usb = warn_console_is_ending,
    .usb_task_core = 0,
};
usb_mic_check_t chk;
esp_err_t err = audio_stream_usb_check(&fmt, &chk);   // USB_MIC_ERR_OVER_BUDGET: chk says what fits
if (err == ESP_OK) {
    err = audio_stream_usb_start(&src, &fmt, &opt);
}
...
audio_stream_usb_stop();
```

## Starting USB takes the PHY

On a chip whose console is USB-Serial-JTAG that ends the console until the next reset. This
component does not warn, silence the log or arm a reset timer, because those are about the
application's console. It calls `before_usb` once the stream's buffers exist and just before
USB starts, which is the last moment the console is still there, and not at all if the
format cannot be streamed.

## The device outlives the stream

If the stream ends by itself (the microphone was closed under it), the USB device stays up
so the host does not see it vanish and reappear, and `audio_stream_usb_start()` refuses a
second start. `audio_stream_usb_stop()` ends it. Read why the stream ended from
`audio_stream_get_stats()`.
