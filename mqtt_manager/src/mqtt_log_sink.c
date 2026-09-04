#include "mqtt_log_sink.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_manager.h"

#include "sdkconfig.h"

#define MAX_SUFFIX CONFIG_MQTT_LOG_SINK_MAX_SUFFIX_LEN

static char s_suffix[MAX_SUFFIX];
static int s_qos;
static bool s_started;
static volatile uint32_t s_dropped;

/*
 * Guards against re-entering the sink on the same task.
 *
 * The hazard is a loop: publishing logs on failure, that log goes to diag, diag
 * calls this sink, which publishes again. It has to be per-task rather than a
 * single flag, because output from an unrelated task must not be dropped merely
 * because another task happens to be inside a publish.
 */
static TaskHandle_t s_in_sink;

static void sink(const char *text, size_t len, void *ctx)
{
    (void)ctx;

    if (!s_started || len == 0) {
        return;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (s_in_sink == self) {
        s_dropped++;
        return;
    }

    if (!mqtt_manager_is_connected()) {
        s_dropped++;
        return;
    }

    s_in_sink = self;
    /*
     * enqueue, not publish: diag holds its output lock across this call, and
     * esp_mqtt_client_publish() blocks until the client task takes the message.
     * Waiting on another task while holding the lock that task needs in order to
     * log is a deadlock; enqueue stores it in the outbox and returns.
     *
     * QoS 0 by default for the same reason a log is not a transaction: losing a
     * line beats stalling the thing that produced it.
     */
    if (mqtt_manager_enqueue(s_suffix, text, len, s_qos, false) != ESP_OK) {
        s_dropped++;
    }
    s_in_sink = NULL;
}

esp_err_t mqtt_log_sink_start(const char *topic_suffix, int qos)
{
    if (!topic_suffix || topic_suffix[0] == '\0' || qos < 0 || qos > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(topic_suffix) >= sizeof(s_suffix)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(s_suffix, topic_suffix, sizeof(s_suffix) - 1);
    s_suffix[sizeof(s_suffix) - 1] = '\0';
    s_qos = qos;

    esp_err_t err = diag_sink_register(sink, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t mqtt_log_sink_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_started = false;
    return diag_sink_unregister(sink, NULL);
}

uint32_t mqtt_log_sink_dropped(void)
{
    return s_dropped;
}
