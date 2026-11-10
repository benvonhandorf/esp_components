/*
 * The runner half of audio_stream: one task that reads the source, decimates a raw PDM
 * source, converts, and calls the sink. See audio_stream.h.
 */

#include "audio_stream.h"

#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pdm_decimator.h"

#define STREAM_BLOCK_MS         5
#define STREAM_MAX_READ_ERRORS  20
#define STREAM_READ_TIMEOUT_MS  100
#define STREAM_TASK_STACK       4096
#define STREAM_TASK_PRIORITY    6

static struct {
    bool active;                  /* from a successful start until it is reaped */
    volatile bool stop;
    volatile bool finished;       /* the task has run its sink's end() and is about to exit */
    bool release_on_end;
    const char *owner;

    audio_stream_source_t src;
    audio_stream_fmt_t fmt;
    audio_stream_sink_t sink;
    void *sink_ctx;

    size_t block_frames;
    size_t in_slot;               /* bytes per slot after decimation: 2 for a raw source */
    pdm_decimator_t *dec;
    uint8_t *raw;                 /* raw source only */
    size_t raw_frames;            /* raw frames per block */
    uint8_t *in;
    uint8_t *out;
} s;

static audio_stream_stats_t s_stats;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Start and stop can come from the console task and from an HTTP handler, so they are
 * serialised. The lock and the exit semaphore are static, made on first use. */
static StaticSemaphore_t s_lock_buf, s_exit_buf;
static SemaphoreHandle_t s_lock, s_exited;

static void init_once(void)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_lock) {
        s_exited = xSemaphoreCreateBinaryStatic(&s_exit_buf);
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    portEXIT_CRITICAL(&s_mux);
}

static void release_buffers(void)
{
    pdm_decimator_destroy(s.dec);
    s.dec = NULL;
    free(s.raw);
    free(s.in);
    free(s.out);
    s.raw = s.in = s.out = NULL;
}

static void stream_task(void *arg)
{
    (void)arg;
    const size_t src_frame_bytes = (size_t)s.src.channels * s.src.slot_bytes;
    audio_stream_end_t why = AUDIO_STREAM_END_STOPPED;
    unsigned consecutive = 0;

    while (!s.stop) {
        if (s.src.alive && !s.src.alive(s.src.ctx)) {
            why = AUDIO_STREAM_END_SOURCE_GONE;
            break;
        }
        size_t frames = 0;
        esp_err_t err;
        uint64_t cycles = 0;
        if (s.dec) {
            /* Undecimated source: raw 16-bit words in, PCM out. One raw frame is one
             * word per channel, the same size as one decimated frame. */
            size_t got = 0;
            err = s.src.read(s.src.ctx, s.raw, s.raw_frames * src_frame_bytes, &got,
                             STREAM_READ_TIMEOUT_MS);
            size_t raw_frames = got / src_frame_bytes;
            if (err == ESP_OK && raw_frames > 0) {
                uint32_t t0 = esp_cpu_get_cycle_count();
                err = pdm_decimator_process(s.dec, (const uint16_t *)s.raw, raw_frames,
                                            (int16_t *)s.in, &frames);
                cycles = (uint32_t)(esp_cpu_get_cycle_count() - t0);
            }
        } else {
            size_t got = 0;
            err = s.src.read(s.src.ctx, s.in, s.block_frames * src_frame_bytes, &got,
                             STREAM_READ_TIMEOUT_MS);
            frames = got / src_frame_bytes;
        }
        if (err != ESP_OK || frames == 0) {
            portENTER_CRITICAL(&s_mux);
            s_stats.read_errors++;
            portEXIT_CRITICAL(&s_mux);
            if (++consecutive >= STREAM_MAX_READ_ERRORS) {
                why = AUDIO_STREAM_END_READ_ERRORS;
                break;
            }
            continue;
        }
        consecutive = 0;

        size_t bytes = audio_stream_convert(&s.fmt, s.in_slot, s.src.channels, s.in, frames, s.out);
        bool alive = s.sink.write(s.sink_ctx, s.out, bytes);

        portENTER_CRITICAL(&s_mux);
        s_stats.frames += frames;
        s_stats.dec_cycles += cycles;
        if (s.dec) {
            s_stats.dec_frames += frames;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!alive) {
            why = AUDIO_STREAM_END_SINK_GONE;
            break;
        }
    }

    /* The task owns its buffers, so it frees them: a stream that ends by itself must not
     * hold a decimator and 20 kB until someone calls stop. */
    release_buffers();
    portENTER_CRITICAL(&s_mux);
    s_stats.running = false;
    s_stats.has_ended = true;
    s_stats.ended = why;
    portEXIT_CRITICAL(&s_mux);
    if (s.sink.end) {
        s.sink.end(s.sink_ctx, why);
    }
    s.finished = true;
    xSemaphoreGive(s_exited);
    vTaskDelete(NULL);
}

/* The task has exited or is about to; wait for it and free the place. Holds s_lock. */
static void reap(void)
{
    xSemaphoreTake(s_exited, portMAX_DELAY);
    s.active = false;
}

esp_err_t audio_stream_start(const audio_stream_source_t *src, const audio_stream_fmt_t *fmt,
                             const audio_stream_sink_t *sink, void *sink_ctx,
                             const audio_stream_options_t *opt)
{
    if (!src || !fmt || !sink || !sink->write || !audio_stream_source_valid(src) ||
        fmt->channels == 0 || fmt->channels > AUDIO_STREAM_MAX_CHANNELS || fmt->rate_hz == 0 ||
        (fmt->bytes != 2 && fmt->bytes != 3) || (src->pdm_raw && fmt->ratio == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < fmt->channels; i++) {
        if (fmt->chan[i] >= src->channels) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    init_once();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s.active) {
        if (s.finished && s.release_on_end) {
            reap();
        } else {
            xSemaphoreGive(s_lock);
            return AUDIO_STREAM_ERR_BUSY;
        }
    }

    const unsigned block_ms = (opt && opt->block_ms) ? opt->block_ms : STREAM_BLOCK_MS;
    const unsigned priority = (opt && opt->priority) ? opt->priority : STREAM_TASK_PRIORITY;
    int core = opt ? opt->core : -1;
    if (core < 0 || core >= (int)configNUM_CORES) {
        core = tskNO_AFFINITY;
    }

    s.src = *src;
    s.fmt = *fmt;
    s.sink = *sink;
    s.sink_ctx = sink_ctx;
    s.release_on_end = sink->release_on_end;
    s.owner = (opt && opt->owner) ? opt->owner : "audio_stream";
    s.in_slot = src->pdm_raw ? 2 : src->slot_bytes;
    s.block_frames = fmt->rate_hz * block_ms / 1000;
    if (s.block_frames < 8) {
        s.block_frames = 8;
    }

    size_t in_frames = s.block_frames;
    esp_err_t err = ESP_OK;
    if (src->pdm_raw) {
        pdm_decimator_config_t dcfg = {
            .filter = PDM_DECIMATOR_SINC6,
            .channels = src->channels,
            .ratio = fmt->ratio,
            .lsb_first = src->pdm_lsb_first,
            .allow_simd = true,
        };
        err = pdm_decimator_create(&dcfg, &s.dec);
        if (err == ESP_OK) {
            s.raw_frames = pdm_decimator_input_frames(s.dec, s.block_frames);
            in_frames = pdm_decimator_max_output(s.dec, s.raw_frames);
            pdm_decimator_reserve(s.dec, s.raw_frames);
            s.raw = malloc(s.raw_frames * (size_t)src->channels * src->slot_bytes);
            if (!s.raw) {
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (err == ESP_OK) {
        s.in = malloc(in_frames * (size_t)src->channels * s.in_slot);
        s.out = malloc(in_frames * fmt->channels * fmt->bytes);
        if (!s.in || !s.out) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        release_buffers();
        xSemaphoreGive(s_lock);
        return err;
    }

    if (sink->begin) {
        err = sink->begin(sink_ctx, fmt);
        if (err != ESP_OK) {
            release_buffers();
            xSemaphoreGive(s_lock);
            return err;
        }
    }

    s.stop = false;
    s.finished = false;
    portENTER_CRITICAL(&s_mux);
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.running = true;
    s_stats.owner = s.owner;
    s_stats.rate_hz = fmt->rate_hz;
    s_stats.channels = fmt->channels;
    s_stats.bytes = fmt->bytes;
    portEXIT_CRITICAL(&s_mux);

    if (xTaskCreatePinnedToCore(stream_task, "audio_stream", STREAM_TASK_STACK, NULL, priority,
                                NULL, core) != pdPASS) {
        release_buffers();
        portENTER_CRITICAL(&s_mux);
        s_stats.running = false;
        portEXIT_CRITICAL(&s_mux);
        if (sink->end) {
            sink->end(sink_ctx, AUDIO_STREAM_END_STOPPED);     /* begin() succeeded: end() follows */
        }
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s.active = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_stream_stop(void)
{
    init_once();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s.active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s.stop = true;
    reap();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *audio_stream_owner(void)
{
    if (!s.active || (s.finished && s.release_on_end)) {
        return NULL;
    }
    return s.owner;
}

void audio_stream_get_stats(audio_stream_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}
