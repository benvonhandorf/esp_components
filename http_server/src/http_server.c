#include "http_server.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "http_auth.h"
#include "net_events.h"

#include "sdkconfig.h"

static const char *TAG = "http_server";

#define MAX_ROUTES CONFIG_HTTP_SERVER_MAX_ROUTES

typedef struct {
    http_route_t route;
    httpd_uri_t uri;   /* kept because httpd stores the pointer we hand it */
} route_entry_t;

static httpd_handle_t s_server;
static http_server_config_t s_cfg;
static bool s_configured;

static route_entry_t s_routes[MAX_ROUTES];
static size_t s_route_count;

/* Built once at start() from our own credentials; the network side is never
 * decoded. See http_auth.h. */
static char s_expected_auth[160];

httpd_handle_t http_server_handle(void) { return s_server; }
bool http_server_is_running(void) { return s_server != NULL; }

void *http_server_route_ctx(httpd_req_t *req)
{
    const route_entry_t *entry = (const route_entry_t *)req->user_ctx;
    return entry ? entry->route.user_ctx : NULL;
}

static void send_challenge(httpd_req_t *req)
{
    char challenge[96];
    snprintf(challenge, sizeof(challenge), "Basic realm=\"%s\"", s_cfg.realm);
    httpd_resp_set_hdr(req, "WWW-Authenticate", challenge);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
}

bool http_server_check_auth(httpd_req_t *req)
{
    if (!s_cfg.require_auth) {
        return true;
    }

    char header[192] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", header,
                                    sizeof(header)) != ESP_OK) {
        send_challenge(req);
        return false;
    }

    if (!http_auth_header_matches(header, s_expected_auth)) {
        ESP_LOGW(TAG, "rejected credentials for %s", req->uri);
        send_challenge(req);
        return false;
    }

    return true;
}

/*
 * Every route goes through here, so the auth decision is made in one place
 * rather than depending on each handler remembering to ask.
 */
static esp_err_t trampoline(httpd_req_t *req)
{
    const route_entry_t *entry = (const route_entry_t *)req->user_ctx;
    if (!entry || !entry->route.handler) {
        return ESP_FAIL;
    }

    if (entry->route.require_auth && !http_server_check_auth(req)) {
        /* The challenge has been sent; the request is complete. */
        return ESP_OK;
    }

    return entry->route.handler(req);
}

static void register_route(route_entry_t *entry)
{
    entry->uri = (httpd_uri_t){
        .uri = entry->route.uri,
        .method = entry->route.method,
        .handler = trampoline,
        .user_ctx = entry,
    };

    esp_err_t err = httpd_register_uri_handler(s_server, &entry->uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registering %s: %s", entry->route.uri, esp_err_to_name(err));
    }
}

esp_err_t http_server_add_routes(const http_route_t *routes, size_t count)
{
    if (!routes && count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_route_count + count > MAX_ROUTES) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count; i++) {
        if (!routes[i].uri || !routes[i].handler) {
            return ESP_ERR_INVALID_ARG;
        }
        route_entry_t *entry = &s_routes[s_route_count++];
        entry->route = routes[i];

        /* Added while already listening: register now rather than making the
         * caller restart the server. */
        if (s_server) {
            register_route(entry);
        }
    }

    return ESP_OK;
}

static esp_err_t start_listening(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = s_cfg.port;
    config.max_open_sockets = s_cfg.max_open_sockets;
    config.lru_purge_enable = true;
    /* Routes are registered from our own table, which may hold more than the
     * httpd default of 8 slots. */
    config.max_uri_handlers = MAX_ROUTES + 4;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        ESP_LOGE(TAG, "starting on port %u: %s", s_cfg.port, esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < s_route_count; i++) {
        register_route(&s_routes[i]);
    }

    ESP_LOGI(TAG, "listening on port %u with %u route(s)%s",
             s_cfg.port, (unsigned)s_route_count,
             s_cfg.require_auth ? ", authenticated" : "");
    return ESP_OK;
}

/*
 * The listener does not need an address, so this is a retry, not the main path.
 *
 * start_listening() binds INADDR_ANY, which is legal with no interface
 * configured and unreachable until one is -- so the server is already listening
 * by the time a link comes up. This stays because the bind can fail for
 * transient reasons (no memory, no socket) at boot, and a link event is a
 * reasonable moment to try again. start_listening() returns immediately when
 * the server is already up, so the usual case costs nothing.
 */
static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    switch ((net_event_id_t)id) {
        case NET_EVENT_LINK_UP:
        case NET_EVENT_AP_STARTED:
            start_listening();
            break;
        default:
            break;
    }
}

esp_err_t http_server_start(const http_server_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;

    if (s_cfg.require_auth) {
        if (s_cfg.password[0] == '\0') {
            /*
             * Refused rather than defaulted. An interface that can reflash the
             * device must not be reachable with credentials that ship in the
             * source, and silently disabling authentication instead would be
             * worse than failing to start.
             */
            ESP_LOGE(TAG, "require_auth is set but no password is configured");
            return HTTP_SERVER_ERR_NO_PASSWORD;
        }
        if (!http_auth_expected_header(s_cfg.username, s_cfg.password,
                                       s_expected_auth, sizeof(s_expected_auth))) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    esp_err_t err = esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID,
                                               &on_net_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_configured = true;

    /*
     * Listen now, rather than waiting for a link.
     *
     * Waiting bought nothing: httpd binds INADDR_ANY, so the socket is not tied
     * to an interface or an address, cannot be reached before one exists, and
     * needs no restart when one changes. What it cost was the handle --
     * http_server_handle() returned NULL for the whole of start-up, so a caller
     * passing it to cli_web to share this server silently got a second server on
     * the same port instead, and this one then failed to listen with EADDRINUSE
     * once the link came up.
     *
     * A failure is still returned, and start_listening() has already said why:
     * on_net_event() will try again, but a caller that meant to share this
     * server must not carry on as though it had one.
     */
    return start_listening();
}

esp_err_t http_server_stop(void)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_event_handler_unregister(NET_EVENT, ESP_EVENT_ANY_ID, &on_net_event);

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    /* The encoded credentials do not outlive the server. */
    memset(s_expected_auth, 0, sizeof(s_expected_auth));
    s_configured = false;
    return ESP_OK;
}
