#include "cli_web.h"

#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "diag.h"

#include "sdkconfig.h"

/* The page is compiled in via EMBED_FILES; no filesystem needed. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

#define MAX_BROADCAST_CHUNK CONFIG_CLI_WEB_BROADCAST_CHUNK
#define MAX_OPEN_SOCKETS    CONFIG_CLI_WEB_MAX_OPEN_SOCKETS
#define MAX_COMMAND_LEN     CONFIG_CLI_WEB_MAX_COMMAND_LEN

static httpd_handle_t server;
static bool owns_server;
static const char *page_uri;
static const char *ws_uri;
static bool page_registered;

/*
 * A chunk of console output on its way to the browsers.
 *
 * httpd_ws_send_frame_async() must run on the HTTP server's own task, so the
 * output sink (which runs on the command executor task) hands the text over via
 * httpd_queue_work() rather than sending directly.
 */
typedef struct {
    size_t len;
    char text[];
} broadcast_t;

static void broadcast_work(void *arg)
{
    broadcast_t *message = arg;

    size_t client_count = MAX_OPEN_SOCKETS;
    int client_fds[MAX_OPEN_SOCKETS];

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

    free(message);
}

static void output_sink(const char *text, size_t len, void *ctx)
{
    (void)ctx;

    if (!server || len == 0) {
        return;
    }

    while (len > 0) {
        size_t chunk = len > MAX_BROADCAST_CHUNK ? MAX_BROADCAST_CHUNK : len;

        broadcast_t *message = malloc(sizeof(broadcast_t) + chunk);
        if (!message) {
            return; /* dropping output beats blocking the shell */
        }

        message->len = chunk;
        memcpy(message->text, text, chunk);

        if (httpd_queue_work(server, broadcast_work, message) != ESP_OK) {
            free(message);
            return;
        }

        text += chunk;
        len -= chunk;
    }
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
         * Hand off without echoing, and without waiting. Calling diag_printf()
         * here would run the output sink -- and therefore httpd_queue_work() --
         * on the HTTP server's own task, risking deadlock against a full
         * control queue. The browser echoes its own input locally instead.
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

    err = diag_sink_register(output_sink, NULL);
    if (err != ESP_OK) {
        goto fail_handlers;
    }

    return ESP_OK;

fail_handlers:
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

    diag_sink_unregister(output_sink, NULL);

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
