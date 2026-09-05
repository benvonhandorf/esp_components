# ota

Over-the-air firmware update to the inactive slot, with rollback.

```c
ota_http_register(NULL);        /* POST /api/ota, authenticated */
```

```sh
curl -u admin:secret -X POST --data-binary @build/firmware.bin \
     http://device.local/api/ota
```

## The transport is separable

`ota.h` takes bytes from wherever they came:

```c
ota_report_t report;
ota_session_t *s;
ota_session_begin(NULL, &s, &report);
ota_session_write(s, chunk, len);      /* repeatedly */
ota_session_end(s);                    /* verify and select for next boot */
```

An update delivered over MQTT or a serial console uses this directly. `ota_http.c` is the
HTTP route for the common case, not the only way in.

## The wrong binary is rejected early

The image header and app descriptor are checked after roughly the first kilobyte, not
after the whole transfer. Uploading the wrong firmware is *the* common accident, and the
error names the project the image was actually built for.

By default an image whose descriptor names a different project is refused. After renaming
`project()` in a project's `CMakeLists.txt`, the next OTA is rejected until the running
firmware is one built under the new name — flash over a cable once to cross that gap.

## Failures name their stage

```c
if (ota_session_end(s) != ESP_OK) {
    printf("%s failed\n", ota_stage_name(report.failed_stage));
}
```

A rejected signature, a partition too small and an interrupted upload need completely
different responses, so "OTA failed" is not enough. `OTA_STAGE_NO_PARTITION` in particular
means the firmware was built with a single-app partition table — an A/B layout is what
makes any of this possible.

## Rollback

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a newly written image boots **on trial** and
is rolled back on the next reset unless it confirms itself:

```c
if (ota_pending_verify()) {
    ota_mark_valid();       /* at the END of start-up, not the beginning */
}
```

Where that call sits *is* the mechanism. An image that crashes during start-up must never
reach it. A device that should prove more than "it started" — that it reached the broker,
say — confirms from there instead.

`ota_rollback_and_reboot()` returns to the previous firmware deliberately.

## Signing

Set `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` and give the project a key; an image
not signed by it then fails at `OTA_STAGE_END`. Without it, anything reaching the
authenticated endpoint is accepted.
