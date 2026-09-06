#include "cli_web.h"

#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

/* The page is compiled in via EMBED_FILES; no filesystem needed. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

#define MAX_BROADCAST_CHUNK CONFIG_CLI_WEB_BROADCAST_CHUNK
#define MAX_OPEN_SOCKETS    CONFIG_CLI_WEB_MAX_OPEN_SOCKETS
#define MAX_COMMAND_LEN     CONFIG_CLI_WEB_MAX_COMMAND_LEN
#define TX_QUEUE_LEN        CONFIG_CLI_WEB_BROADCAST_QUEUE_LEN
#define TX_STACK            CONFIG_CLI_WEB_TX_STACK

/*
 * A ceiling that no server's client list can exceed, borrowed or owned: httpd
 * refuses to start unless max_open_sockets + 3 fits in CONFIG_LWIP_MAX_SOCKETS.
 *
 * Sizing this from CONFIG_CLI_WEB_MAX_OPEN_SOCKETS was wrong for a borrowed
 * server, which keeps whatever its owner configured -- and
 * httpd_get_client_list() does not truncate to the space offered, it fails the
 * whole call. One connection past our number and output stopped reaching every
 * browser, not just the extra one.
 */
#if defined(CONFIG_LWIP_MAX_SOCKETS)
#define MAX_CLIENT_FDS CONFIG_LWIP_MAX_SOCKETS
#else
#define MAX_CLIENT_FDS 15 /* what esp_http_server itself assumes off-target */
#endif

static httpd_handle_t server;
static bool owns_server;
static const char *page_uri;
static const char *ws_uri;
static bool page_registered;

/* Output on its way to the browsers, and the task that delivers it. See
 * output_sink() for why the hop through a task is not optional. */
static QueueHandle_t tx_queue;
static SemaphoreHandle_t tx_stopped;
static TaskHandle_t tx_task;
static volatile uint32_t tx_dropped;

/* The task inside broadcast_work(), while it is in there. See output_sink(). */
static volatile TaskHandle_t sending_task;

/*
 * A chunk of console output on its way to the browsers.
 *
 * httpd_ws_send_frame_async() must run on the HTTP server's own task, so the
 * text is handed over via httpd_queue_work() rather than sent directly.
 */
typedef struct {
    size_t len;
    char text[];
} broadcast_t;

static void broadcast_work(void *arg)
{
    broadcast_t *message = arg;

    size_t client_count = MAX_CLIENT_FDS;
    int client_fds[MAX_CLIENT_FDS];

    /*
     * Claim the task for as long as we are sending. A failed send logs, and that
     * line must not become the next thing we try to send -- see output_sink().
     * A plain assignment is enough because httpd runs one server task, so
     * broadcast_work() never overlaps itself.
     */
    sending_task = xTaskGetCurrentTaskHandle();

    if (httpd_get_client_list(server, &client_count, client_fds) == ESP_OK) {
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)message->text,
            .len = message->len,
        };

        for (size_t i = 0; i < client_count; i++) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(server, client_fds[i], &frame);
            }
        }
    }

    sending_task = NULL;
    free(message);
}

/*
 * Drains the queue and hands each chunk to the HTTP server's task.
 *
 * This task exists solely to own the socket call. httpd_queue_work() sends a
 * datagram to the server's control socket, and with LWIP_TCPIP_CORE_LOCKING
 * disabled every socket call posts to the TCP/IP mailbox and waits for the
 * TCP/IP thread to run it.
 */
static void broadcaster_task(void *arg)
{
    (void)arg;

    broadcast_t *message = NULL;

    while (xQueueReceive(tx_queue, &message, portMAX_DELAY) == pdTRUE) {
        if (!message) {
            break; /* stop sentinel from cli_web_stop() */
        }

        /* broadcast_work() frees it on the server task; we free it only when it
         * never gets there. httpd_queue_work() logs its own failure, which comes
         * straight back to output_sink() -- see the guard there. */
        if (!server || httpd_queue_work(server, broadcast_work, message) != ESP_OK) {
            free(message);
            tx_dropped++;
        }
    }

    xSemaphoreGive(tx_stopped);
    vTaskDelete(NULL);
}

/*
 * Copy the text onto the queue and return. Nothing here blocks, and nothing
 * here enters lwIP.
 *
 * That is the whole point of this function, and the reason the queue exists.
 * diag calls sinks with its output lock held, on whatever task produced the
 * line -- which includes lwIP's TCP/IP thread, because ESP_LOGx from an lwIP
 * raw-API callback (the SNTP time-sync notification, for one) runs there.
 * Calling httpd_queue_work() directly meant that thread posted to its own
 * mailbox and waited for itself: a permanent deadlock, holding diag's lock, so
 * the next diag_printf() on any task -- the shell's first line of output --
 * never returned either.
 */
static void output_sink(const char *text, size_t len, void *ctx)
{
    (void)ctx;

    if (!tx_queue || len == 0) {
        return;
    }

    /*
     * Never queue what our own delivery path logged. Two tasks are inside it and
     * both of them log on failure:
     *
     * - the broadcaster, where httpd_queue_work() warns "failed to queue work"
     *   once the server's control queue is full;
     * - the server task, where httpd_ws_send_frame_async() warns "Failed to send
     *   WS header" or "... payload" for a socket that has gone away.
     *
     * Either line arrives here on the task that produced it. Queued, it goes
     * back down the same path, fails the same way and logs again -- a loop that
     * feeds itself for as long as one browser's socket stays broken, and the
     * second case sustains it even while the first never fires. The serial
     * console still gets those lines, because diag writes there directly.
     */
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (self == tx_task || self == sending_task) {
        tx_dropped++;
        return;
    }

    while (len > 0) {
        size_t chunk = len > MAX_BROADCAST_CHUNK ? MAX_BROADCAST_CHUNK : len;

        broadcast_t *message = malloc(sizeof(broadcast_t) + chunk);
        if (!message) {
            tx_dropped++;
            return; /* dropping output beats blocking the thing that logged it */
        }

        message->len = chunk;
        memcpy(message->text, text, chunk);

        /* Zero ticks: a browser that cannot keep up must not stall the console,
         * the shell, or the TCP/IP thread. */
        if (xQueueSend(tx_queue, &message, 0) != pdTRUE) {
            free(message);
            tx_dropped++;
            return;
        }

        text += chunk;
        len -= chunk;
    }
}

uint32_t cli_web_dropped(void)
{
    return tx_dropped;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Opening handshake; nothing to read yet. */
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};

    /* Called with max_len 0 this only fills in the length. */
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0 || frame.len > MAX_COMMAND_LEN) {
        return ESP_OK;
    }

    uint8_t *payload = calloc(1, frame.len + 1);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    frame.payload = payload;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err == ESP_OK) {
        /*
         * Hand off without echoing, and without waiting: cli_submit() with
         * wait=false, so the server task is free to serve the next request
         * rather than sitting out however long the command takes. The browser
         * echoes its own input locally instead.
         */
        cli_submit((char *)payload, false);
    }

    free(payload);
    return err;
}

httpd_handle_t cli_web_server(void)
{
    return server;
}

/*
 * Wind the broadcaster down and free what it used. Safe at any stage of
 * cli_web_start(), which is why it is one function rather than a ladder of
 * labels: the sink must already be unregistered, so nothing can add to the
 * queue while this runs.
 *
 * portMAX_DELAY is safe here and nowhere else in this file: this runs on a
 * caller's task, never on the TCP/IP thread.
 */
static void stop_broadcaster(void)
{
    if (tx_task) {
        broadcast_t *sentinel = NULL;
        xQueueSend(tx_queue, &sentinel, portMAX_DELAY);
        xSemaphoreTake(tx_stopped, portMAX_DELAY);
        tx_task = NULL;
    }

    if (tx_queue) {
        /* Whatever the task did not reach. */
        broadcast_t *message;
        while (xQueueReceive(tx_queue, &message, 0) == pdTRUE) {
            free(message);
        }
        vQueueDelete(tx_queue);
        tx_queue = NULL;
    }

    if (tx_stopped) {
        vSemaphoreDelete(tx_stopped);
        tx_stopped = NULL;
    }
}

esp_err_t cli_web_start(const cli_web_config_t *cfg)
{
    if (server) {
        return ESP_ERR_INVALID_STATE;
    }

    const cli_web_config_t defaults = {.serve_page = true};
    if (!cfg) {
        cfg = &defaults;
    }

    page_uri = cfg->page_uri ? cfg->page_uri : "/";
    ws_uri   = cfg->ws_uri   ? cfg->ws_uri   : "/ws";

    if (cfg->server) {
        server = cfg->server;
        owns_server = false;
    } else {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.lru_purge_enable = true;
        config.server_port = cfg->port ? cfg->port : 80;
        /* Every browser holds a WebSocket open, so the default of 4 sockets
         * runs out quickly with a couple of tabs. */
        config.max_open_sockets = MAX_OPEN_SOCKETS;

        esp_err_t err = httpd_start(&server, &config);
        if (err != ESP_OK) {
            server = NULL;
            return err;
        }
        owns_server = true;
    }

    const httpd_uri_t ws = {
        .uri = ws_uri,
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };

    esp_err_t err = httpd_register_uri_handler(server, &ws);
    if (err != ESP_OK) {
        goto fail;
    }

    if (cfg->serve_page) {
        const httpd_uri_t page = {
            .uri = page_uri,
            .method = HTTP_GET,
            .handler = index_handler,
        };
        err = httpd_register_uri_handler(server, &page);
        if (err != ESP_OK) {
            httpd_unregister_uri_handler(server, ws_uri, HTTP_GET);
            goto fail;
        }
        page_registered = true;
    }

    /* Everything the sink touches must exist before it can be called, and it
     * can be called by any task the moment it is registered. */
    tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(broadcast_t *));
    tx_stopped = xSemaphoreCreateBinary();
    if (!tx_queue || !tx_stopped) {
        err = ESP_ERR_NO_MEM;
        goto fail_tx;
    }

    tx_dropped = 0;
    if (xTaskCreate(broadcaster_task, "cli_web_tx", TX_STACK, NULL, 3, &tx_task) != pdPASS) {
        tx_task = NULL;
        err = ESP_ERR_NO_MEM;
        goto fail_tx;
    }

    err = diag_sink_register(output_sink, NULL);
    if (err != ESP_OK) {
        goto fail_tx;
    }

    return ESP_OK;

fail_tx:
    stop_broadcaster();
    if (page_registered) {
        httpd_unregister_uri_handler(server, page_uri, HTTP_GET);
        page_registered = false;
    }
    httpd_unregister_uri_handler(server, ws_uri, HTTP_GET);
fail:
    if (owns_server) {
        httpd_stop(server);
    }
    server = NULL;
    owns_server = false;
    return err;
}

esp_err_t cli_web_stop(void)
{
    if (!server) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Unregister first: after this returns no task is inside output_sink(), so
     * nothing can queue work behind the sentinel. */
    diag_sink_unregister(output_sink, NULL);
    stop_broadcaster();

    if (page_registered) {
        httpd_unregister_uri_handler(server, page_uri, HTTP_GET);
        page_registered = false;
    }
    httpd_unregister_uri_handler(server, ws_uri, HTTP_GET);

    /* Only stop what we started: a borrowed server belongs to the caller. */
    if (owns_server) {
        httpd_stop(server);
    }

    server = NULL;
    owns_server = false;
    return ESP_OK;
}
