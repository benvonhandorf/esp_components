#pragma once

/*
 * A microphone as an HTTP response: a WAV file with no end, which `curl | ffplay`, VLC, sox
 * and a browser's <audio> element all play as it arrives.
 *
 * This is a helper for a handler, not a server and not a route: the application owns its
 * route table and its authentication (a microphone is private; protect the route), and calls
 * audio_stream_http_serve() from the handler. That is also why this depends on ESP-IDF's
 * esp_http_server and on nothing else of ours.
 *
 * ONE LISTENER. A second request while one is being served is refused, not queued.
 *
 * THE HANDLER RETURNS AT ONCE. serve() takes an asynchronous copy of the request
 * (httpd_req_async_handler_begin()) and a task of this component's sends the audio, so the
 * server's single task -- shared with whatever else the application serves -- is never held
 * for the length of a recording. The server does not close or purge a socket that is in an
 * asynchronous request.
 *
 * A BUFFER BETWEEN. The stream task writes into an audio_stream_queue, which a drain task
 * empties into the socket: a WiFi stall costs a gap in the audio, never an overrun in the
 * receiver. With external RAM the buffer can hold seconds. See audio_stream_queue.h.
 */

#include <stdbool.h>
#include <stdint.h>

#include "audio_stream.h"
#include "audio_stream_queue.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    audio_stream_options_t stream;       /* owner NULL -> "http" */
    /* The buffer: bytes (0 -> 65536), use_psram, reserve_internal_bytes, chunk_bytes and
     * flush_ms. name, core, priority and stack_bytes are this component's to set, and are
     * ignored. */
    audio_stream_queue_config_t queue;
} audio_stream_http_options_t;

/*
 * Read ?rate=48000&bits=24&ch=0,1 from the request into `want`. Only the keys present are
 * changed, so fill `want` with the application's defaults first. Errors are
 * AUDIO_STREAM_ERR_BAD_RATE, _BAD_BITS, _BAD_LIST, _BAD_CHANNEL and _DUP_CHANNEL, with the
 * facts in `why` (may be NULL). Does not check a rate against the microphone: that is
 * audio_stream_resolve().
 */
esp_err_t audio_stream_http_parse_query(httpd_req_t *req, const audio_stream_source_t *src,
                                        audio_stream_want_t *want, audio_stream_why_t *why);

/*
 * Serve `fmt` (from audio_stream_resolve()) as the response to `req`.
 *
 * ESP_OK: the stream is running and this component now owns the response; return ESP_OK
 * from the handler and send nothing more. Anything else: nothing has been sent, and the
 * handler should answer itself, typically 409 for AUDIO_STREAM_ERR_BUSY and 500 otherwise.
 *
 * AUDIO_STREAM_ERR_BUSY means another listener, another sink holding the microphone, or a
 * previous listener still closing: audio_stream_owner() says which of the first two.
 */
esp_err_t audio_stream_http_serve(httpd_req_t *req, const audio_stream_source_t *src,
                                  const audio_stream_fmt_t *fmt,
                                  const audio_stream_http_options_t *opt);

/* End the HTTP stream, if there is one: ESP_ERR_INVALID_STATE when there is none, or the
 * microphone is being streamed by something else. The client sees the response end. */
esp_err_t audio_stream_http_stop(void);

typedef struct {
    bool active;                  /* a listener is being served */
    char peer[48];                /* the listener's address; empty if unknown */
    uint64_t bytes_sent;          /* including the WAV header */
    uint32_t elapsed_ms;          /* since it began; frozen when it ends */
    bool send_failed;             /* it ended because the listener went away */
    audio_stream_queue_stats_t queue;
} audio_stream_http_stats_t;

/* The current listener, or the most recent one. */
void audio_stream_http_get_stats(audio_stream_http_stats_t *out);

#ifdef __cplusplus
}
#endif
