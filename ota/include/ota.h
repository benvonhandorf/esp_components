#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Over-the-air firmware update: write an image to the inactive slot, then boot
 * it, with the previous firmware kept so a bad update can be undone.
 *
 * The transport is not part of this. ota_session_* takes bytes from wherever
 * they came -- HTTP, MQTT, a serial console -- and ota_http.h adds an HTTP POST
 * route for the common case.
 */

#define OTA_ERR_BASE 0x36000
/* The image is not a firmware image for this chip, caught from its header before
 * anything is written. */
#define OTA_ERR_NOT_AN_IMAGE   (OTA_ERR_BASE + 1)
/* The image is for a different project, caught from its app descriptor. */
#define OTA_ERR_WRONG_PROJECT  (OTA_ERR_BASE + 2)
/* Fewer bytes arrived than a header needs. */
#define OTA_ERR_TRUNCATED      (OTA_ERR_BASE + 3)

/* Which step failed, so a caller can say what to do rather than "OTA failed".
 * The distinction matters: a rejected signature and a full partition need
 * completely different responses. */
typedef enum {
    OTA_STAGE_NONE = 0,
    OTA_STAGE_NO_PARTITION,   /* no inactive slot -- a single-app partition table */
    OTA_STAGE_BEGIN,          /* erasing the target slot */
    OTA_STAGE_VALIDATE,       /* the image header or app descriptor */
    OTA_STAGE_WRITE,          /* writing, e.g. the image is larger than the slot */
    OTA_STAGE_END,            /* checksum, and signature if verification is on */
    OTA_STAGE_SET_BOOT,       /* updating the boot selection */
} ota_stage_t;

typedef struct {
    ota_stage_t failed_stage;
    esp_err_t   err;          /* the underlying error, if any */
    size_t      bytes_written;
    char        partition[17];      /* label of the slot being written */
    char        image_project[33];  /* project name from the incoming image */
    char        image_version[33];  /* version from the incoming image */
} ota_report_t;

typedef struct ota_session ota_session_t;

typedef struct {
    /*
     * Refuse an image whose app descriptor names a different project.
     *
     * Uploading the wrong firmware is the common accident, and without this the
     * device takes it, reboots, and comes back as something else -- or does not
     * come back. Default on; clear it deliberately when renaming a project.
     */
    bool require_matching_project;
} ota_config_t;

/*
 * Begin an update. `cfg` may be NULL for defaults.
 *
 * The report is filled in on every path, success or failure, and is valid until
 * the session is finished or aborted.
 */
esp_err_t ota_session_begin(const ota_config_t *cfg, ota_session_t **out,
                            ota_report_t *report);

/*
 * Write the next chunk. The first call also validates the image header and app
 * descriptor, so a wrong or corrupt image is rejected after roughly the first
 * kilobyte rather than after the whole upload.
 */
esp_err_t ota_session_write(ota_session_t *session, const void *data, size_t len);

/*
 * Finish: verify the image and select it for the next boot. Does not reboot --
 * a caller usually has a response to send first.
 *
 * With signature verification enabled in the build
 * (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT), an image not signed by the
 * project's key fails here, at OTA_STAGE_END.
 */
esp_err_t ota_session_end(ota_session_t *session);

/* Abandon an update and free the session. Safe at any point. */
void ota_session_abort(ota_session_t *session);

/*
 * Mark the running firmware as good, cancelling the pending rollback.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, firmware that boots and never
 * calls this is rolled back on the next reset. Call it once the device has
 * demonstrated it works -- the network is up, the sensors answer -- not from the
 * first line of app_main(), which would defeat the mechanism.
 */
esp_err_t ota_mark_valid(void);

/* Whether the running firmware is awaiting confirmation. */
bool ota_pending_verify(void);

/* Roll back to the previous firmware and reboot. */
esp_err_t ota_rollback_and_reboot(void);

/* Human-readable stage name, for reporting. Never NULL. */
const char *ota_stage_name(ota_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
