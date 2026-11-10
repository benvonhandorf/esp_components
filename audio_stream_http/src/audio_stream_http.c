/*
 * A microphone as an endless WAV response. See audio_stream_http.h.
 *
 * Who touches what. The handler task runs serve(), which sets up the state below and starts
 * the stream. The stream task calls write() and, once, end(). The drain task owns the async
 * request: it sends on it and completes it. `h.orig` is the handler's request, valid only
 * until serve() returns; `h.req` is the async copy.
 *
 * There is one of everything, because there is one listener, and it is reused. That is safe
 * only because serve() refuses to start while the previous queue is still in use (see
 * acquire()): the previous drain task is gone by the time anything here is rewritten.
 */

#include "audio_stream_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define DEFAULT_BUFFER_BYTES 65536
#define DRAIN_STACK_BYTES    6144       /* httpd_resp_send_chunk() runs on it */

static struct {
    bool starting;                      /* serve() is between acquire and return */
    httpd_req_t *orig;                  /* the handler's; valid during serve() only */
    httpd_req_t *req;                   /* the async copy, until the drain task completes it */
    bool committed;                     /* begin() succeeded: the response is ours */
    bool header_sent;                   /* drain task only */
    const char *owner;
    audio_stream_fmt_t fmt;
    audio_stream_queue_config_t qcfg;
    audio_stream_queue_t queue;

    /* Statistics, under s_mux. */
    bool serving;
    char peer[48];
    uint64_t bytes_sent;
    int64_t t0_us, t_end_us;
    bool send_failed;
} h;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- the query ------------------------------------------------------------------------- */

static esp_err_t fail(audio_stream_why_t *why, esp_err_t code)
{
    why->code = code;
    return code;
}

/* ESP_OK with val filled, ESP_ERR_NOT_FOUND when the key is absent, anything else for a
 * value that does not fit (a silently ignored key would be worse than a refusal). */
static esp_err_t query_value(const char *q, const char *key, char *val, size_t size)
{
    return httpd_query_key_value(q, key, val, size);
}

static bool parse_positive(const char *text, int *out)
{
    char *end;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v <= 0 || v > 10000000) {
        return false;
    }
    *out = (int)v;
    return true;
}

/* A comma may arrive as %2C: curl leaves it alone, browsers and URL builders do not. */
static void decode_commas(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (r[0] == '%' && r[1] == '2' && (r[2] == 'C' || r[2] == 'c')) {
            *w++ = ',';
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

esp_err_t audio_stream_http_parse_query(httpd_req_t *req, const audio_stream_source_t *src,
                                        audio_stream_want_t *want, audio_stream_why_t *why)
{
    audio_stream_why_t local;
    if (!why) {
        why = &local;
    }
    memset(why, 0, sizeof *why);
    if (!req || !src || !want) {
        return fail(why, ESP_ERR_INVALID_ARG);
    }

    char q[128];
    esp_err_t e = httpd_req_get_url_query_str(req, q, sizeof q);
    if (e == ESP_ERR_NOT_FOUND) {
        return ESP_OK;                                  /* no query: the defaults stand */
    }
    if (e != ESP_OK) {
        return fail(why, AUDIO_STREAM_ERR_BAD_LIST);    /* longer than any valid one */
    }

    char val[48];
    int v;
    e = query_value(q, "rate", val, sizeof val);
    if (e == ESP_OK) {
        if (!parse_positive(val, &v)) {
            return fail(why, AUDIO_STREAM_ERR_BAD_RATE);
        }
        want->rate = (uint32_t)v;
    } else if (e != ESP_ERR_NOT_FOUND) {
        return fail(why, AUDIO_STREAM_ERR_BAD_RATE);
    }

    e = query_value(q, "bits", val, sizeof val);
    if (e == ESP_OK) {
        if (!parse_positive(val, &v) || (v != 16 && v != 24)) {
            why->src_bits = src->data_bits;
            return fail(why, AUDIO_STREAM_ERR_BAD_BITS);
        }
        want->bits = (uint8_t)v;
    } else if (e != ESP_ERR_NOT_FOUND) {
        return fail(why, AUDIO_STREAM_ERR_BAD_BITS);
    }

    e = query_value(q, "ch", val, sizeof val);
    if (e == ESP_OK) {
        decode_commas(val);
        int n = 0;
        esp_err_t pe = audio_stream_parse_channels(val, src->channels, want->chan, &n, why);
        if (pe != ESP_OK) {
            return pe;
        }
        want->nch = n;
    } else if (e != ESP_ERR_NOT_FOUND) {
        return fail(why, AUDIO_STREAM_ERR_BAD_LIST);
    }
    why->code = ESP_OK;
    return ESP_OK;
}

/* ---- the listener ---------------------------------------------------------------------- */

static void add_sent(size_t n)
{
    portENTER_CRITICAL(&s_mux);
    h.bytes_sent += n;
    portEXIT_CRITICAL(&s_mux);
}

static void peer_name(httpd_req_t *r, char *out, size_t size)
{
    out[0] = '\0';
    int fd = httpd_req_to_sockfd(r);
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&ss, &len) != 0) {
        return;
    }
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, out, (socklen_t)size);
    } else if (ss.ss_family == AF_INET6) {
        const struct in6_addr *a = &((struct sockaddr_in6 *)&ss)->sin6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(a->s6_addr, mapped, sizeof mapped) == 0) {
            /* An IPv4 peer on a dual-stack socket: show it as the address it is. */
            inet_ntop(AF_INET, &a->s6_addr[12], out, (socklen_t)size);
        } else {
            inet_ntop(AF_INET6, a, out, (socklen_t)size);
        }
    }
}

/* Drain task. Blocks until the data is sent; an error means the listener is gone. */
static esp_err_t http_send(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!h.header_sent) {
        uint8_t hdr[AUDIO_STREAM_WAV_HEADER_MAX];
        size_t n = audio_stream_wav_header(&h.fmt, hdr);
        if (httpd_resp_send_chunk(h.req, (const char *)hdr, (ssize_t)n) != ESP_OK) {
            return ESP_FAIL;
        }
        h.header_sent = true;
        add_sent(n);
    }
    if (httpd_resp_send_chunk(h.req, (const char *)data, (ssize_t)len) != ESP_OK) {
        return ESP_FAIL;
    }
    add_sent(len);
    return ESP_OK;
}

/* Drain task, after its last send. Ends the response and gives the socket back. */
static void http_done(void *ctx, bool failed)
{
    (void)ctx;
    httpd_req_t *r = h.req;
    if (!r) {
        return;                         /* begin() gave up before the request was taken */
    }
    if (failed) {
        /* The listener is gone mid-chunk: nothing valid can follow, so close the socket
         * rather than leave the server to find out. */
        httpd_handle_t handle = r->handle;
        int fd = httpd_req_to_sockfd(r);
        httpd_req_async_handler_complete(r);
        httpd_sess_trigger_close(handle, fd);
    } else {
        if (!h.header_sent) {           /* stopped before any audio: still a valid, empty WAV */
            uint8_t hdr[AUDIO_STREAM_WAV_HEADER_MAX];
            size_t n = audio_stream_wav_header(&h.fmt, hdr);
            if (httpd_resp_send_chunk(r, (const char *)hdr, (ssize_t)n) == ESP_OK) {
                add_sent(n);
            }
        }
        httpd_resp_send_chunk(r, NULL, 0);
        httpd_req_async_handler_complete(r);
    }
    h.req = NULL;
    portENTER_CRITICAL(&s_mux);
    h.serving = false;
    h.send_failed = failed;
    h.t_end_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
}

/* ---- the sink -------------------------------------------------------------------------- */

static esp_err_t http_begin(void *ctx, const audio_stream_fmt_t *fmt)
{
    (void)ctx;
    (void)fmt;
    const audio_stream_queue_out_t out = {.send = http_send, .done = http_done};
    esp_err_t err = audio_stream_queue_init(&h.queue, &h.qcfg, &out);
    if (err == ESP_ERR_INVALID_STATE) {
        return AUDIO_STREAM_ERR_BUSY;   /* the previous listener's queue is still closing */
    }
    if (err != ESP_OK) {
        return err;
    }

    httpd_req_t *copy = NULL;
    err = httpd_req_async_handler_begin(h.orig, &copy);
    if (err != ESP_OK) {
        audio_stream_queue_finish(&h.queue);    /* the drain task sees it finished and ends */
        return err;
    }
    httpd_resp_set_type(copy, "audio/wav");
    httpd_resp_set_hdr(copy, "Cache-Control", "no-store");

    char peer[sizeof h.peer];
    peer_name(copy, peer, sizeof peer);
    portENTER_CRITICAL(&s_mux);
    memcpy(h.peer, peer, sizeof peer);
    h.bytes_sent = 0;
    h.send_failed = false;
    h.serving = true;
    h.t0_us = esp_timer_get_time();
    h.t_end_us = 0;
    portEXIT_CRITICAL(&s_mux);
    h.header_sent = false;
    h.req = copy;
    h.committed = true;                 /* from here the response is ours */
    return ESP_OK;
}

static bool http_write(void *ctx, const uint8_t *pcm, size_t len)
{
    (void)ctx;
    return audio_stream_queue_write(&h.queue, pcm, len);
}

static void http_end(void *ctx, audio_stream_end_t why)
{
    (void)ctx;
    (void)why;
    audio_stream_queue_finish(&h.queue);        /* the drain task flushes what is left, then ends */
}

/* One listener, and the previous one fully gone: serve() may not touch the shared state
 * while a drain task could still be using it. */
static bool acquire(void)
{
    audio_stream_queue_stats_t qs;
    audio_stream_queue_get_stats(&h.queue, &qs);
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (!h.starting && !qs.active) {
        h.starting = true;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

esp_err_t audio_stream_http_serve(httpd_req_t *req, const audio_stream_source_t *src,
                                  const audio_stream_fmt_t *fmt,
                                  const audio_stream_http_options_t *opt)
{
    if (!req || !src || !fmt) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!acquire()) {
        return AUDIO_STREAM_ERR_BUSY;
    }

    audio_stream_options_t so = {0};
    audio_stream_queue_config_t qc = {0};
    if (opt) {
        so = opt->stream;
        qc = opt->queue;
    }
    if (!so.owner) {
        so.owner = "http";
    }
    if (!qc.bytes) {
        qc.bytes = DEFAULT_BUFFER_BYTES;
    }
    qc.name = "aq_http";
    qc.core = -1;
    qc.priority = 0;
    qc.stack_bytes = DRAIN_STACK_BYTES;

    h.orig = req;
    h.fmt = *fmt;
    h.qcfg = qc;
    h.owner = so.owner;
    h.committed = false;

    const audio_stream_sink_t sink = {
        .begin = http_begin,
        .write = http_write,
        .end = http_end,
        .release_on_end = true,         /* a listener that went away frees the microphone */
    };
    esp_err_t err = audio_stream_start(src, fmt, &sink, NULL, &so);

    h.orig = NULL;
    bool committed = h.committed;
    portENTER_CRITICAL(&s_mux);
    h.starting = false;
    portEXIT_CRITICAL(&s_mux);
    if (err != ESP_OK && committed) {
        return ESP_OK;                  /* begin() succeeded and the task did not: end() has
                                         * run, and the drain task answers the request */
    }
    return err;
}

esp_err_t audio_stream_http_stop(void)
{
    const char *owner = audio_stream_owner();
    if (!owner || owner != h.owner) {
        return ESP_ERR_INVALID_STATE;
    }
    return audio_stream_stop();
}

void audio_stream_http_get_stats(audio_stream_http_stats_t *out)
{
    memset(out, 0, sizeof *out);
    audio_stream_queue_get_stats(&h.queue, &out->queue);
    portENTER_CRITICAL(&s_mux);
    out->active = h.serving;
    memcpy(out->peer, h.peer, sizeof out->peer);
    out->bytes_sent = h.bytes_sent;
    out->send_failed = h.send_failed;
    int64_t end = h.serving ? esp_timer_get_time() : h.t_end_us;
    out->elapsed_ms = (h.t0_us && end > h.t0_us) ? (uint32_t)((end - h.t0_us) / 1000) : 0;
    portEXIT_CRITICAL(&s_mux);
}
