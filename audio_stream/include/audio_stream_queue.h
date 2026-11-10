#pragma once

/*
 * A buffer between the stream task and a sink that can block -- a TCP socket, a file --
 * and the task that drains it.
 *
 * WHY. The source is a receiver that overruns if it is not read every few milliseconds,
 * and a socket send can stall for seconds when WiFi does. The queue lets the producer
 * hand over a block and return at once: when the buffer is full the block is dropped and
 * counted, so a network stall costs a gap in the recording and never an overrun in the
 * receiver. A bigger buffer rides out longer stalls, and turns a stall into lasting delay
 * for a live listener: that trade is the caller's, set in milliseconds by the caller.
 *
 * WHERE. On a chip with external RAM the buffer goes there when `use_psram` is set: a
 * second of 4-channel 48 kHz audio is 384 kB, which internal RAM cannot spare beside
 * WiFi. Without it the buffer is internal, and is capped by the largest free block less
 * `reserve_internal_bytes`, so asking for more than fits shrinks the buffer instead of
 * starving lwIP. The achieved size is in the statistics. The drain task copies each chunk
 * into an internal buffer before sending, so the network stack never reads external RAM.
 *
 * Single producer (the stream task), single consumer (the drain task), no lock.
 *
 * LIFETIME. The queue is a caller-owned struct, usually a static: a project streams one
 * thing at a time, and the statistics then stay readable after the stream is gone. It is
 * in use from audio_stream_queue_init() until both sides are done -- the producer calls
 * audio_stream_queue_finish(), and the drain task exits, either because the producer
 * finished and the buffer emptied, or because send() failed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t bytes;                  /* wanted capacity */
    bool use_psram;                /* prefer external RAM when the chip has it */
    size_t reserve_internal_bytes; /* internal heap to leave free; 0 -> 40960 */
    size_t chunk_bytes;            /* send at least this much at a time...; 0 -> 4096 */
    uint32_t flush_ms;             /* ...or whatever has arrived after this long; 0 -> 20 */
    const char *name;              /* the drain task's; NULL -> "aq_drain" */
    int core;                      /* -1 for any */
    unsigned priority;             /* 0 -> 5 */
    uint32_t stack_bytes;          /* 0 -> 4096 */
} audio_stream_queue_config_t;

typedef struct {
    /* Block until `len` bytes are sent. Anything but ESP_OK means the peer is gone: the
     * drain task stops, and audio_stream_queue_write() returns false from then on. */
    esp_err_t (*send)(void *ctx, const uint8_t *data, size_t len);
    /* Optional. Called once in the drain task after its last send(), whether it drained
     * the buffer or gave up, with the final statistics. */
    void (*done)(void *ctx, bool send_failed);
    void *ctx;
} audio_stream_queue_out_t;

typedef struct {
    uint64_t bytes_in;             /* accepted into the buffer */
    uint64_t bytes_out;            /* handed to send() and sent */
    uint64_t bytes_dropped;        /* refused because the buffer was full */
    uint32_t drops;                /* blocks refused */
    size_t capacity_bytes;         /* achieved, which may be less than asked */
    size_t fill_bytes;             /* held now */
    size_t high_water_bytes;       /* the most it has held */
    bool in_psram;
    bool send_failed;              /* send() returned an error */
    bool active;                   /* in use: the drain task is alive, or the producer is not done */
} audio_stream_queue_stats_t;

/* Private fields. A struct rather than an opaque handle so the caller can make it static. */
typedef struct {
    audio_stream_queue_config_t cfg;
    audio_stream_queue_out_t out;
    audio_stream_queue_stats_t st;
    void *ring;                    /* the audio_stream_ring_t */
    uint8_t *store;
    uint8_t *chunk;
    volatile bool producer_done;
    volatile bool closed;
    int refs;
} audio_stream_queue_t;

/*
 * Allocate the buffer and start the drain task. ESP_ERR_INVALID_STATE if `q` is still in
 * use from a previous stream, ESP_ERR_NO_MEM if there is no room for even a small buffer.
 */
esp_err_t audio_stream_queue_init(audio_stream_queue_t *q, const audio_stream_queue_config_t *cfg,
                                  const audio_stream_queue_out_t *out);

/* Producer side. Never blocks: copies the block in and returns true, or drops the whole
 * block (a partial one would shift the channels) and returns true, or returns false when
 * the drain task has given up and the stream should end. */
bool audio_stream_queue_write(audio_stream_queue_t *q, const uint8_t *data, size_t len);

/* The producer is done. The drain task sends what is left, then ends; the memory is freed
 * when both sides are. */
void audio_stream_queue_finish(audio_stream_queue_t *q);

/* Safe from any task, whether or not the queue is in use. */
void audio_stream_queue_get_stats(const audio_stream_queue_t *q, audio_stream_queue_stats_t *out);

#ifdef __cplusplus
}
#endif
