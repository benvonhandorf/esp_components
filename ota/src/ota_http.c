#include "ota_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "ota.h"

#include "sdkconfig.h"

static const char *TAG = "ota_http";

#define CHUNK CONFIG_OTA_HTTP_CHUNK_SIZE
/* Long enough for the response to reach the client before the reboot cuts the
 * connection. Shorter than a client would wait, longer than a flush needs. */
#define REBOOT_DELAY_MS 750

static void send_failure(httpd_req_t *req, const ota_report_t *report, esp_err_t err)
{
    char message[192];

    /* Say which step failed and what to do about it. "OTA failed" leaves the
     * user unable to tell a wrong binary from a full partition. */
    switch (report->failed_stage) {
        case OTA_STAGE_NO_PARTITION:
            snprintf(message, sizeof(message),
                     "No OTA partition: this firmware was built with a "
                     "single-app partition table.");
            break;
        case OTA_STAGE_VALIDATE:
            if (err == OTA_ERR_WRONG_PROJECT) {
                snprintf(message, sizeof(message),
                         "Image is for project '%s', this device runs a different "
                         "project.", report->image_project);
            } else if (err == OTA_ERR_NOT_AN_IMAGE) {
                snprintf(message, sizeof(message),
                         "Not a firmware image. Upload the .bin from the build "
                         "directory, not the .elf or a compressed archive.");
            } else {
                snprintf(message, sizeof(message),
                         "Upload ended after %u bytes, before a complete image "
                         "header.", (unsigned)report->bytes_written);
            }
            break;
        case OTA_STAGE_WRITE:
            snprintf(message, sizeof(message),
                     "Write failed after %u bytes (%s). The image may be larger "
                     "than the '%s' partition.",
                     (unsigned)report->bytes_written, esp_err_to_name(err),
                     report->partition);
            break;
        case OTA_STAGE_END:
            snprintf(message, sizeof(message),
                     "Image verification failed (%s). It may be corrupt, or not "
                     "signed by this project's key.", esp_err_to_name(err));
            break;
        default:
            snprintf(message, sizeof(message), "%s failed: %s",
                     ota_stage_name(report->failed_stage), esp_err_to_name(err));
            break;
    }

    ESP_LOGE(TAG, "%s", message);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, message);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    ota_report_t report;
    ota_session_t *session = NULL;

    esp_err_t err = ota_session_begin(NULL, &session, &report);
    if (err != ESP_OK) {
        send_failure(req, &report, err);
        return ESP_OK;   /* the error response is the answer */
    }

    static char buf[CHUNK];
    int received;

    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        err = ota_session_write(session, buf, (size_t)received);
        if (err != ESP_OK) {
            ota_session_abort(session);
            send_failure(req, &report, err);
            return ESP_OK;
        }
    }

    if (received < 0) {
        /* The upload was cut short: a dropped connection, or a timeout. */
        ota_session_abort(session);
        ESP_LOGE(TAG, "receive failed after %u bytes",
                 (unsigned)report.bytes_written);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Upload interrupted");
        return ESP_OK;
    }

    err = ota_session_end(session);
    if (err != ESP_OK) {
        send_failure(req, &report, err);
        return ESP_OK;
    }

    char done[160];
    snprintf(done, sizeof(done),
             "Update staged on '%s': %s %s, %u bytes. Rebooting.\n",
             report.partition, report.image_project, report.image_version,
             (unsigned)report.bytes_written);
    httpd_resp_sendstr(req, done);

    /* Reboot after the response, not inside it: rebooting first means the client
     * is told the update succeeded only by never hearing back. */
    ESP_LOGI(TAG, "rebooting into the new image");
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();

    return ESP_OK;   /* unreachable */
}

esp_err_t ota_http_register(const char *uri)
{
    static http_route_t route;

    route = (http_route_t){
        .uri = uri ? uri : "/api/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
        /* Anything that can replace the firmware is protected, always. */
        .require_auth = true,
    };

    return http_server_add_routes(&route, 1);
}
