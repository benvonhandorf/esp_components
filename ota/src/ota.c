#include "ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "ota";

/* Enough of the image to hold the header, the segment header and the app
 * descriptor, which is what validation needs before committing to a write. */
#define HEADER_BYTES (sizeof(esp_image_header_t) +      \
                      sizeof(esp_image_segment_header_t) + \
                      sizeof(esp_app_desc_t))

struct ota_session {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    ota_report_t *report;
    ota_config_t cfg;
    bool validated;
    uint8_t header[HEADER_BYTES];
    size_t header_len;
};

const char *ota_stage_name(ota_stage_t stage)
{
    switch (stage) {
        case OTA_STAGE_NONE:         return "none";
        case OTA_STAGE_NO_PARTITION: return "no update partition";
        case OTA_STAGE_BEGIN:        return "preparing the partition";
        case OTA_STAGE_VALIDATE:     return "validating the image";
        case OTA_STAGE_WRITE:        return "writing";
        case OTA_STAGE_END:          return "verifying the image";
        case OTA_STAGE_SET_BOOT:     return "selecting the boot partition";
    }
    return "unknown";
}

static esp_err_t fail(ota_session_t *s, ota_stage_t stage, esp_err_t err)
{
    if (s->report) {
        s->report->failed_stage = stage;
        s->report->err = err;
    }
    return err;
}

esp_err_t ota_session_begin(const ota_config_t *cfg, ota_session_t **out,
                            ota_report_t *report)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (report) {
        memset(report, 0, sizeof(*report));
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        /* A partition table with a single app slot, which the template's A/B
         * layout is specifically meant to avoid. */
        if (report) {
            report->failed_stage = OTA_STAGE_NO_PARTITION;
            report->err = ESP_ERR_NOT_FOUND;
        }
        return ESP_ERR_NOT_FOUND;
    }

    ota_session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }

    s->partition = target;
    s->report = report;
    s->cfg = cfg ? *cfg : (ota_config_t){.require_matching_project = true};
    if (!cfg) {
        s->cfg.require_matching_project = true;
    }

    if (report) {
        snprintf(report->partition, sizeof(report->partition), "%s", target->label);
    }

    /* OTA_WITH_SEQUENTIAL_WRITES erases as it goes, so a device with no PSRAM
     * need not buffer the image and the long up-front erase disappears. */
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &s->handle);
    if (err != ESP_OK) {
        fail(s, OTA_STAGE_BEGIN, err);
        free(s);
        return err;
    }

    ESP_LOGI(TAG, "writing to '%s' at 0x%08" PRIx32, target->label, target->address);
    *out = s;
    return ESP_OK;
}

/*
 * Check the image header and app descriptor before the upload goes any further.
 *
 * Rejecting after roughly a kilobyte instead of after the whole transfer matters
 * on a device: the common accident is uploading the wrong binary, and the cost of
 * finding out late is a slot erased for nothing and, if it were accepted, a
 * device that reboots into something else.
 */
static esp_err_t validate_header(ota_session_t *s)
{
    if (s->header_len < HEADER_BYTES) {
        return fail(s, OTA_STAGE_VALIDATE, OTA_ERR_TRUNCATED);
    }

    const esp_image_header_t *img = (const esp_image_header_t *)s->header;
    if (img->magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "not a firmware image (magic 0x%02x)", img->magic);
        return fail(s, OTA_STAGE_VALIDATE, OTA_ERR_NOT_AN_IMAGE);
    }

    const esp_app_desc_t *desc = (const esp_app_desc_t *)
        (s->header + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));

    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "image carries no app descriptor");
        return fail(s, OTA_STAGE_VALIDATE, OTA_ERR_NOT_AN_IMAGE);
    }

    if (s->report) {
        snprintf(s->report->image_project, sizeof(s->report->image_project),
                 "%.*s", (int)sizeof(desc->project_name), desc->project_name);
        snprintf(s->report->image_version, sizeof(s->report->image_version),
                 "%.*s", (int)sizeof(desc->version), desc->version);
    }

    if (s->cfg.require_matching_project) {
        const esp_app_desc_t *running = esp_app_get_description();
        if (running && strncmp(running->project_name, desc->project_name,
                               sizeof(desc->project_name)) != 0) {
            ESP_LOGE(TAG, "image is for project '%.*s', this is '%s'",
                     (int)sizeof(desc->project_name), desc->project_name,
                     running->project_name);
            return fail(s, OTA_STAGE_VALIDATE, OTA_ERR_WRONG_PROJECT);
        }
    }

    ESP_LOGI(TAG, "image: project '%.*s' version '%.*s'",
             (int)sizeof(desc->project_name), desc->project_name,
             (int)sizeof(desc->version), desc->version);

    s->validated = true;
    return ESP_OK;
}

esp_err_t ota_session_write(ota_session_t *s, const void *data, size_t len)
{
    if (!s || (!data && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    /* Accumulate until there is enough to validate. The bytes are still written
     * through in the same call, so nothing is buffered beyond the header. */
    if (!s->validated && s->header_len < HEADER_BYTES) {
        size_t want = HEADER_BYTES - s->header_len;
        size_t take = len < want ? len : want;
        memcpy(s->header + s->header_len, data, take);
        s->header_len += take;

        if (s->header_len == HEADER_BYTES) {
            esp_err_t err = validate_header(s);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    esp_err_t err = esp_ota_write(s->handle, data, len);
    if (err != ESP_OK) {
        return fail(s, OTA_STAGE_WRITE, err);
    }

    if (s->report) {
        s->report->bytes_written += len;
    }
    return ESP_OK;
}

esp_err_t ota_session_end(ota_session_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }

    /* An upload that ended before a full header never got validated, and is
     * certainly not a firmware image. */
    if (!s->validated) {
        esp_err_t err = fail(s, OTA_STAGE_VALIDATE, OTA_ERR_TRUNCATED);
        esp_ota_abort(s->handle);
        free(s);
        return err;
    }

    esp_err_t err = esp_ota_end(s->handle);
    if (err != ESP_OK) {
        /* Also where a bad signature surfaces, when the build verifies them. */
        ESP_LOGE(TAG, "image verification failed: %s", esp_err_to_name(err));
        fail(s, OTA_STAGE_END, err);
        free(s);
        return err;
    }

    err = esp_ota_set_boot_partition(s->partition);
    if (err != ESP_OK) {
        fail(s, OTA_STAGE_SET_BOOT, err);
        free(s);
        return err;
    }

    ESP_LOGI(TAG, "update staged on '%s'; it boots on the next restart",
             s->partition->label);
    free(s);
    return ESP_OK;
}

void ota_session_abort(ota_session_t *s)
{
    if (!s) {
        return;
    }
    esp_ota_abort(s->handle);
    free(s);
}

esp_err_t ota_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

bool ota_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t ota_rollback_and_reboot(void)
{
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
