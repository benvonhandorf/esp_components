#include "diag.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"

#define MAX_SINKS       CONFIG_DIAG_MAX_SINKS
#define FORMAT_BUF_SIZE CONFIG_DIAG_FORMAT_BUF_SIZE

typedef struct {
    diag_sink_fn fn;
    void *ctx;
} sink_t;

static sink_t sinks[MAX_SINKS];
static size_t sink_count;
static SemaphoreHandle_t output_mutex;

/* Output is formatted once into this buffer, then handed to stdout and every
 * sink, so all interfaces see byte-identical text. Guarded by output_mutex. */
static char format_buf[FORMAT_BUF_SIZE];

/* ESP_LOGx output is routed here so log lines reach every interface too. */
static int log_vprintf(const char *fmt, va_list args)
{
    diag_vprintf(fmt, args);
    return 0;
}

esp_err_t diag_init(const diag_config_t *cfg)
{
    if (output_mutex) {
        return ESP_OK;
    }

    output_mutex = xSemaphoreCreateRecursiveMutex();
    if (!output_mutex) {
        return ESP_ERR_NO_MEM;
    }

    if (!cfg || cfg->capture_esp_log) {
        esp_log_set_vprintf(log_vprintf);
    }
    return ESP_OK;
}

esp_err_t diag_sink_register(diag_sink_fn fn, void *ctx)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!output_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;

    xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    if (sink_count == MAX_SINKS) {
        err = ESP_ERR_NO_MEM;
    } else {
        sinks[sink_count].fn = fn;
        sinks[sink_count].ctx = ctx;
        sink_count++;
    }
    xSemaphoreGiveRecursive(output_mutex);

    return err;
}

esp_err_t diag_sink_unregister(diag_sink_fn fn, void *ctx)
{
    if (!output_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;

    xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    for (size_t i = 0; i < sink_count; i++) {
        if (sinks[i].fn == fn && sinks[i].ctx == ctx) {
            sinks[i] = sinks[sink_count - 1];
            sink_count--;
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGiveRecursive(output_mutex);

    return err;
}

void diag_write(const char *text, size_t len)
{
    if (len == 0) {
        return;
    }

    /* diag_init() may not have run yet during very early startup. */
    if (output_mutex) {
        xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    }

    fwrite(text, 1, len, stdout);

    for (size_t i = 0; i < sink_count; i++) {
        sinks[i].fn(text, len, sinks[i].ctx);
    }

    if (output_mutex) {
        xSemaphoreGiveRecursive(output_mutex);
    }
}

void diag_vprintf(const char *fmt, va_list args)
{
    if (output_mutex) {
        xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    }

    int len = vsnprintf(format_buf, sizeof(format_buf), fmt, args);
    if (len > 0) {
        /* vsnprintf() reports the length it *would* have written. */
        size_t written = (size_t)len < sizeof(format_buf) ? (size_t)len
                                                          : sizeof(format_buf) - 1;
        diag_write(format_buf, written);
    }

    if (output_mutex) {
        xSemaphoreGiveRecursive(output_mutex);
    }
}

void diag_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    diag_vprintf(fmt, args);
    va_end(args);
}

void diag_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    /* Held across all three writes so the prefix, the message and the newline
     * cannot be split by output from another task. */
    if (output_mutex) {
        xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    }

    diag_printf("ERR: ");
    diag_vprintf(fmt, args);
    diag_printf("\n");

    if (output_mutex) {
        xSemaphoreGiveRecursive(output_mutex);
    }

    va_end(args);
}
