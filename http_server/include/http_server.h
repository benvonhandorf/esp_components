#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "http_server_config.h"   /* generated from the schema */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HTTP management interface: one server, routes registered as data, and optional
 * Basic authentication per route.
 *
 * Start it at boot, before the network: httpd binds INADDR_ANY, so the listener
 * needs no address, cannot be reached until an interface has one, and needs no
 * restart when one changes. http_server_handle() is therefore valid as soon as
 * start() returns, which is what lets cli_web share this server rather than
 * opening a second listener on the same port.
 *
 * It also subscribes to NET_EVENT_LINK_UP and NET_EVENT_AP_STARTED and retries
 * there, so a bind that failed at boot for want of memory still comes up. Being
 * reachable over the device's own access point is when a management interface
 * matters most: a device that joined nothing is the one you need to reconfigure.
 */

#define HTTP_SERVER_ERR_BASE 0x35000
/* Authentication is enabled and no password is configured. Refused rather than
 * defaulted, so a fleet cannot ship with well-known credentials. */
#define HTTP_SERVER_ERR_NO_PASSWORD (HTTP_SERVER_ERR_BASE + 1)

typedef struct {
    const char *uri;                       /* e.g. "/api/status" */
    httpd_method_t method;                 /* HTTP_GET, HTTP_POST, ... */
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;                        /* retrieve with http_server_route_ctx() */
    /*
     * Demand credentials before the handler runs.
     *
     * Per route rather than per server: a status endpoint a dashboard scrapes and
     * an endpoint that reflashes the device do not warrant the same treatment.
     */
    bool require_auth;
} http_route_t;

/*
 * Register routes. The array must stay valid for the life of the process --
 * pass a static.
 *
 * This replaces a weak `http_server_register_services()` that applications
 * overrode with a strong symbol: that depended on link order, and gave a
 * component no way to serve a route of its own. Routes may be added before or
 * after start(); those added first are registered when the server comes up.
 *
 * ESP_ERR_NO_MEM if the table is full (CONFIG_HTTP_SERVER_MAX_ROUTES).
 */
esp_err_t http_server_add_routes(const http_route_t *routes, size_t count);

/*
 * ESP_ERR_INVALID_ARG for a NULL config, HTTP_SERVER_ERR_NO_PASSWORD if
 * authentication is on with an empty password.
 */
esp_err_t http_server_start(const http_server_config_t *cfg);
esp_err_t http_server_stop(void);

/* The running server, or NULL. Pass it to cli_web so the console shares this
 * server rather than opening a second listener. */
httpd_handle_t http_server_handle(void);

/* Whether the server is listening. */
bool http_server_is_running(void);

/* The user_ctx the current route was registered with. */
void *http_server_route_ctx(httpd_req_t *req);

/*
 * Check credentials from inside a handler that did not set require_auth.
 *
 * Sends 401 with a WWW-Authenticate header and returns false when they are
 * missing or wrong, so a handler should return ESP_OK immediately.
 */
bool http_server_check_auth(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVER_H */
