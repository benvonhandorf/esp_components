# usb_mic

A USB Audio Class 2 **microphone** for chips with a USB OTG controller (ESP32-S2, S3,
P4, S31). Feed it interleaved PCM with `usb_mic_write()`; the host sees a standard USB
microphone (`arecord`, Audacity, a browser).

```c
usb_mic_device_config_t cfg = {
    .stream = {.rate_hz = 48000, .channels = 2, .bytes = 2},
    .task_core = 0,
};
usb_mic_check_t check;
esp_err_t err = usb_mic_start(&cfg, &check);   // USB_MIC_ERR_OVER_BUDGET if it does not fit
...
usb_mic_write(pcm, bytes);                      // never blocks
```

## What fits

A packet must fit whole in the endpoint's transmit FIFO, and the controller has one
block of FIFO RAM for every FIFO. On the ESP32-S2/S3 (256 words) that leaves **712 bytes
per 1 ms packet**, shared by all channels: `(floor(rate/1000) + 1) x channels x bytes`.
`usb_mic_check()`, `usb_mic_max_channels()` and `usb_mic_max_rate()` answer from the
controller they are given, so a caller can validate a configuration before USB starts and
say what would fit. They are plain C with a host test (`make -C test`).

| Rate | 16-bit channels on an S3 |
|---|---|
| 300 kHz | 1 |
| 192 kHz | 1 |
| 150 kHz | 2 |
| 48 kHz | 7 |
| 32 kHz | 8 |

## Things to know

- **Starting it takes the USB PHY.** On a chip whose console is USB-Serial-JTAG, the
  console ends until reset.
- **It installs its own TinyUSB configuration** (audio class only, from `tusb/`), the way
  `espressif/usb_device_uac` does, so it cannot share TinyUSB with `esp_tinyusb`'s CDC
  console or another device class.
- **Packet sizes are TinyUSB's flow control**, not this component's: it sends n-1, n or
  n+1 frames per packet from the buffer's fill, which absorbs the drift between the PCM
  clock and the host's frame clock. It takes the rate from the clock source's GET_CUR reply.
- **No feature unit**, so no mute or volume controls.
- Needs `espressif/tinyusb` ^0.21.0~4 (declared in `idf_component.yml`).
