#include "mqtt_log_sink.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "mqtt_manager.h"

#include "sdkconfig.h"

#define MAX_SUFFIX CONFIG_MQTT_LOG_SINK_MAX_SUFFIX_LEN

static char s_suffix[MAX_SUFFIX];
static int s_qos;
static bool s_started;
static volatile uint32_t s_dropped;

/*
 * The sink never touches the MQTT client. diag calls it with its output lock held,
 * and esp_mqtt_client_enqueue() takes the client's API lock with no timeout. The
 * MQTT task holds that lock while it logs -- a transport error when the link drops
 * is the usual case -- and logging needs the output lock. Each side then waits for
 * the other: the console, the web console and every log line stop for good, which is
 * what `wifi off` from a connected state did.
 *
 * So the sink only copies the text into a message buffer, which never blocks, and a
 * task of its own does the publishing with no lock of diag's held.
 */
#define BUFFER_BYTES   2048
#define CHUNK_BYTES    240      /* one message; longer text is split */
#define TASK_STACK     3072

static MessageBufferHandle_t s_buffer;
static TaskHandle_t s_task;

static void sink(const char *text, size_t len, void *ctx)
{
    (void)ctx;

    if (!s_started || len == 0 || !s_buffer) {
        return;
    }

    /* Nothing to publish over; do not fill the buffer with what cannot be sent. */
    if (!mqtt_manager_is_connected()) {
        s_dropped++;
        return;
    }

    while (len > 0) {
        size_t n = len < CHUNK_BYTES ? len : CHUNK_BYTES;
        if (xMessageBufferSend(s_buffer, text, n, 0) != n) {
            s_dropped++;
        }
        text += n;
        len -= n;
    }
}

static void publisher_task(void *arg)
{
    (void)arg;
    char chunk[CHUNK_BYTES];

    for (;;) {
        size_t n = xMessageBufferReceive(s_buffer, chunk, sizeof(chunk), portMAX_DELAY);
        if (n == 0 || !s_started || !mqtt_manager_is_connected()) {
            continue;
        }
        if (mqtt_manager_enqueue(s_suffix, chunk, n, s_qos, false) != ESP_OK) {
            s_dropped++;
        }
    }
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

    if (!s_buffer) {
        s_buffer = xMessageBufferCreate(BUFFER_BYTES);
        if (!s_buffer) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_task &&
        xTaskCreate(publisher_task, "mqtt_log", TASK_STACK, NULL, tskIDLE_PRIORITY + 1,
                    &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

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
