#ifndef CLI_WEB_H
#define CLI_WEB_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Browser transport for the cli shell.
 *
 * Serves a single-page terminal and accepts command lines over a WebSocket.
 * Lines from the browser go onto the same queue the serial reader feeds, and a
 * diag sink mirrors all command output to every connected browser -- so output
 * from a command typed on the serial port appears in the browser and vice
 * versa, which is the entire point.
 *
 * Start it once the device has an address.
 */

typedef struct {
    /*
     * An HTTP server to register handlers on. NULL starts and owns one.
     *
     * A device with its own web interface already has a server; two listeners
     * on one device is a bug, not a feature. Passing yours also lets you keep
     * authentication, CORS and error handling in one place. cli_web_stop()
     * stops the server only if it started it.
     *
     * A borrowed server must have CONFIG_HTTPD_WS_SUPPORT enabled and enough
     * max_open_sockets: every open browser tab holds a WebSocket for as long as
     * it is on the page.
     */
    httpd_handle_t server;

    /* Only used when starting our own server. 0 selects port 80. */
    uint16_t port;

    /* URI for the console page. NULL selects "/". Ignored if !serve_page. */
    const char *page_uri;

    /*
     * URI for the WebSocket. NULL selects "/ws".
     *
     * The built-in page connects to "/ws" specifically; if you change this,
     * serve your own page.
     */
    const char *ws_uri;

    /* Serve the built-in console page. Set false to supply your own UI and
     * use only the WebSocket. */
    bool serve_page;
} cli_web_config_t;

/*
 * Register the handlers and begin mirroring output. `cfg` may be NULL, which
 * means: start our own server on port 80, serve the built-in page at "/", and
 * accept commands at "/ws".
 *
 * ESP_ERR_INVALID_STATE if already started.
 */
esp_err_t cli_web_start(const cli_web_config_t *cfg);

/* Stop mirroring and unregister the handlers. Stops the HTTP server only if
 * cli_web_start() created it. */
esp_err_t cli_web_stop(void);

/* The server in use, borrowed or owned. NULL if not started. */
httpd_handle_t cli_web_server(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_WEB_H */
