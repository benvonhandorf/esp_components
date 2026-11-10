/*
 * The queue between the stream task and a sink that can block. See audio_stream_queue.h.
 *
 * The drain task polls rather than waiting on a notification from the producer: a
 * notification needs the drain task's handle, and the drain task can exit (the peer went
 * away) while the producer is about to notify it. Polling at one tick costs nothing and
 * has no such window.
 */

#include "audio_stream_queue.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_stream_ring.h"

#define DEFAULT_RESERVE_INTERNAL 40960
#define DEFAULT_CHUNK            4096
#define DEFAULT_FLUSH_MS         20
#define DEFAULT_PRIORITY         5
#define DEFAULT_STACK            4096

/* One lock for every queue: it guards a few counters, taken once per block. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void free_memory(audio_stream_queue_t *q)
{
    free(q->ring);
    heap_caps_free(q->store);
    heap_caps_free(q->chunk);
    q->ring = NULL;
    q->store = NULL;
    q->chunk = NULL;
}

/* Drops one of the two references (producer, drain task); the last one frees. */
static void release_ref(audio_stream_queue_t *q)
{
    portENTER_CRITICAL(&s_mux);
    bool last = (--q->refs == 0);
    portEXIT_CRITICAL(&s_mux);
    if (last) {
        free_memory(q);
        portENTER_CRITICAL(&s_mux);
        q->st.fill_bytes = 0;
        q->st.active = false;
        portEXIT_CRITICAL(&s_mux);
    }
}

static void drain_task(void *arg)
{
    audio_stream_queue_t *q = arg;
    audio_stream_ring_t *ring = q->ring;
    const TickType_t flush = pdMS_TO_TICKS(q->cfg.flush_ms) ? pdMS_TO_TICKS(q->cfg.flush_ms) : 1;
    TickType_t last_send = xTaskGetTickCount();
    bool failed = false;

    for (;;) {
        size_t avail = audio_stream_ring_fill(ring);
        bool done = q->producer_done;
        bool due = avail >= q->cfg.chunk_bytes ||
                   (avail > 0 && (done || xTaskGetTickCount() - last_send >= flush));
        if (!due) {
            if (done) {
                break;                    /* finished and empty */
            }
            vTaskDelay(1);
            continue;
        }
        size_t n = audio_stream_ring_read(ring, q->chunk, q->cfg.chunk_bytes);
        portENTER_CRITICAL(&s_mux);
        q->st.fill_bytes = audio_stream_ring_fill(ring);
        portEXIT_CRITICAL(&s_mux);

        if (q->out.send(q->out.ctx, q->chunk, n) != ESP_OK) {
            failed = true;
            break;
        }
        last_send = xTaskGetTickCount();
        portENTER_CRITICAL(&s_mux);
        q->st.bytes_out += n;
        portEXIT_CRITICAL(&s_mux);
    }

    portENTER_CRITICAL(&s_mux);
    q->st.send_failed = failed;
    portEXIT_CRITICAL(&s_mux);
    q->closed = true;                     /* the producer's next write() says so */
    if (q->out.done) {
        q->out.done(q->out.ctx, failed);
    }
    release_ref(q);
    vTaskDelete(NULL);
}

/* Capacity and where: external RAM when asked for and present, else internal, shrunk
 * so the heap keeps `reserve` bytes for everything else. Returns NULL for no room. */
static uint8_t *alloc_store(const audio_stream_queue_config_t *cfg, size_t *bytes, bool *psram)
{
    const size_t min = 2 * (size_t)cfg->chunk_bytes;
    size_t want = cfg->bytes;

    if (cfg->use_psram && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t n = want < largest ? want : largest;
        if (n >= min) {
            uint8_t *p = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p) {
                *bytes = n;
                *psram = true;
                return p;
            }
        }
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t room = largest > cfg->reserve_internal_bytes ? largest - cfg->reserve_internal_bytes : 0;
    size_t n = want < room ? want : room;
    if (n < min) {
        return NULL;
    }
    uint8_t *p = heap_caps_malloc(n + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) {
        *bytes = n;
        *psram = false;
    }
    return p;
}

esp_err_t audio_stream_queue_init(audio_stream_queue_t *q, const audio_stream_queue_config_t *cfg,
                                  const audio_stream_queue_out_t *out)
{
    if (!q || !cfg || !out || !out->send || cfg->bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    if (q->refs != 0) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    q->refs = 2;                          /* the producer and the drain task */
    memset(&q->st, 0, sizeof q->st);
    q->st.active = true;
    portEXIT_CRITICAL(&s_mux);

    q->cfg = *cfg;
    if (!q->cfg.reserve_internal_bytes) {
        q->cfg.reserve_internal_bytes = DEFAULT_RESERVE_INTERNAL;
    }
    if (!q->cfg.chunk_bytes) {
        q->cfg.chunk_bytes = DEFAULT_CHUNK;
    }
    if (!q->cfg.flush_ms) {
        q->cfg.flush_ms = DEFAULT_FLUSH_MS;
    }
    if (!q->cfg.name) {
        q->cfg.name = "aq_drain";
    }
    if (!q->cfg.priority) {
        q->cfg.priority = DEFAULT_PRIORITY;
    }
    if (!q->cfg.stack_bytes) {
        q->cfg.stack_bytes = DEFAULT_STACK;
    }
    if (q->cfg.core < 0 || q->cfg.core >= (int)configNUM_CORES) {
        q->cfg.core = tskNO_AFFINITY;
    }
    q->out = *out;
    q->producer_done = false;
    q->closed = false;

    size_t bytes = 0;
    bool psram = false;
    q->store = alloc_store(&q->cfg, &bytes, &psram);
    q->chunk = heap_caps_malloc(q->cfg.chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    audio_stream_ring_t *ring = malloc(sizeof *ring);
    if (!q->store || !q->chunk || !ring) {
        free(ring);
        free_memory(q);
        portENTER_CRITICAL(&s_mux);
        q->refs = 0;
        q->st.active = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }
    q->ring = ring;
    audio_stream_ring_init(ring, q->store, bytes + 1);
    portENTER_CRITICAL(&s_mux);
    q->st.capacity_bytes = bytes;
    q->st.in_psram = psram;
    portEXIT_CRITICAL(&s_mux);

    if (xTaskCreatePinnedToCore(drain_task, q->cfg.name, q->cfg.stack_bytes, q, q->cfg.priority,
                                NULL, q->cfg.core) != pdPASS) {
        free_memory(q);
        portENTER_CRITICAL(&s_mux);
        q->refs = 0;
        q->st.active = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool audio_stream_queue_write(audio_stream_queue_t *q, const uint8_t *data, size_t len)
{
    if (q->closed) {
        return false;
    }
    audio_stream_ring_t *ring = q->ring;
    bool ok = audio_stream_ring_write(ring, data, len);
    size_t fill = audio_stream_ring_fill(ring);
    portENTER_CRITICAL(&s_mux);
    if (ok) {
        q->st.bytes_in += len;
    } else {
        q->st.bytes_dropped += len;
        q->st.drops++;
    }
    q->st.fill_bytes = fill;
    if (fill > q->st.high_water_bytes) {
        q->st.high_water_bytes = fill;
    }
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void audio_stream_queue_finish(audio_stream_queue_t *q)
{
    q->producer_done = true;
    release_ref(q);
}

void audio_stream_queue_get_stats(const audio_stream_queue_t *q, audio_stream_queue_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = q->st;
    portEXIT_CRITICAL(&s_mux);
}
