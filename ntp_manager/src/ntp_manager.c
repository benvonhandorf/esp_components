#include "ntp_manager.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "net_events.h"

static const char *TAG = "ntp_manager";

static ntp_manager_config_t s_cfg;
static bool s_started;      /* subscribed to link events */
static bool s_sntp_running; /* esp_netif_sntp_init() has been called */
static bool s_synced;
static struct timeval s_last_sync;

/*
 * Called by lwIP's SNTP client when the clock has been set.
 *
 * This runs on the TCP/IP thread: lwIP's SNTP is a raw-API UDP client, so
 * sntp_recv() -> sntp_process() -> sntp_sync_time() reaches here with no task
 * hop in between. Record, post, return -- and in particular do not log.
 *
 * ESP_LOGx here would be routed into diag, which calls its sinks with its
 * output lock held on the calling task. A sink that touches a socket then makes
 * the TCP/IP thread post to its own mailbox and wait for itself, deadlocking it
 * with diag's lock held, which takes every interface on the device down with
 * it. The line is logged from the NET_EVENT_TIME_SYNCED handler below instead,
 * which runs on the event loop task.
 */
static void on_time_sync(struct timeval *tv)
{
    s_synced = true;
    s_last_sync = *tv;

    net_events_post(NET_EVENT_TIME_SYNCED, tv, sizeof(*tv));
}

static void start_sntp(void)
{
    if (s_sntp_running) {
        /* A reconnect: the servers may now resolve differently (a .local name
         * cannot be looked up until mDNS is up), so restart rather than leaving
         * a client that failed its first resolution retrying a stale address. */
        esp_netif_sntp_deinit();
        s_sntp_running = false;
    }

    /*
     * esp_sntp_config_t::servers is sized by CONFIG_LWIP_SNTP_MAX_SERVERS, whose
     * default is 1 -- so a second server is not merely ignored, writing it would
     * run off the end of the struct. Use only what the build actually has room
     * for, and say what to change rather than silently dropping the fallback.
     */
    bool want_fallback = (s_cfg.server_fallback[0] != '\0');
    size_t count = 1;
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    if (want_fallback) {
        count = 2;
    }
#else
    if (want_fallback) {
        ESP_LOGW(TAG, "ignoring server_fallback '%s': this build allows one NTP "
                      "server (raise CONFIG_LWIP_SNTP_MAX_SERVERS)",
                 s_cfg.server_fallback);
    }
#endif

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_cfg.server);
    sntp_cfg.num_of_servers = count;
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    if (count > 1) {
        sntp_cfg.servers[1] = s_cfg.server_fallback;
    }
#endif
    sntp_cfg.sync_cb = on_time_sync;
    sntp_cfg.start = true;
    /* The default config asks for a semaphore to block on; the callback above is
     * how this component learns about a sync, so nothing would ever wait on it. */
    sntp_cfg.wait_for_sync = false;

    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting SNTP: %s", esp_err_to_name(err));
        return;
    }

    s_sntp_running = true;
    esp_sntp_set_sync_interval(s_cfg.sync_interval_ms);
    ESP_LOGI(TAG, "SNTP started against %s", s_cfg.server);
}

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch ((net_event_id_t)id) {
        case NET_EVENT_LINK_UP:
            start_sntp();
            break;
        case NET_EVENT_LINK_DOWN:
            /* Leave the client running: esp-sntp retries on its own, and tearing
             * it down here would lose the sync interval on every brief drop. */
            break;
        case NET_EVENT_TIME_SYNCED: {
            /* Our own event, come back round on the event loop task. This is
             * where the sync is announced, because on_time_sync() may not. */
            const struct timeval *tv = data;
            ESP_LOGI(TAG, "clock set from NTP: %lld", (long long)tv->tv_sec);
            break;
        }
        default:
            break;
    }
}

esp_err_t ntp_manager_start(const ntp_manager_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;

    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "disabled by configuration");
        return ESP_OK;
    }

    /*
     * Set the timezone before the first sync, not after: localtime() reads TZ at
     * call time, so a record timestamped between the sync and a later setenv
     * would be formatted in the wrong zone.
     */
    if (s_cfg.timezone[0] != '\0') {
        setenv("TZ", s_cfg.timezone, 1);
        tzset();
    }

    esp_err_t err = esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID,
                                               &on_net_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "waiting for the network before synchronising");
    return ESP_OK;
}

esp_err_t ntp_manager_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_event_handler_unregister(NET_EVENT, ESP_EVENT_ANY_ID, &on_net_event);

    if (s_sntp_running) {
        esp_netif_sntp_deinit();
        s_sntp_running = false;
    }

    s_started = false;
    return ESP_OK;
}

bool ntp_manager_is_synced(void)
{
    return s_synced;
}

esp_err_t ntp_manager_last_sync(struct timeval *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_last_sync;
    return ESP_OK;
}
