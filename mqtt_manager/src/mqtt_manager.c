#include "mqtt_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt_topic.h"
#include "net_events.h"

#include "sdkconfig.h"

static const char *TAG = "mqtt_manager";

#define MAX_SUBSCRIPTIONS CONFIG_MQTT_MANAGER_MAX_SUBSCRIPTIONS
#define MAX_TOPIC         160

typedef struct {
    char filter[MAX_TOPIC];   /* full topic filter, prefix already applied */
    int qos;
    mqtt_message_cb_t cb;
    void *ctx;
} subscription_t;

static esp_mqtt_client_handle_t s_client;
static bool s_connected;        /* the client's session: MQTT_EVENT_(DIS)CONNECTED */
static bool s_running;          /* esp_mqtt_client_start() called */
static bool s_link_up;          /* the network under it: NET_EVENT_LINK_UP/DOWN */

static mqtt_manager_config_t s_cfg;
static char s_prefix[MAX_TOPIC];   /* $DEVICE$ already substituted */
static char s_lwt_topic[MAX_TOPIC];

static subscription_t s_subs[MAX_SUBSCRIPTIONS];
static size_t s_sub_count;

const char *mqtt_manager_topic_prefix(void)
{
    return s_client ? s_prefix : NULL;
}

/*
 * Two facts, owned by two sources, combined only here.
 *
 * s_connected used to be cleared on NET_EVENT_LINK_DOWN as well, so status would
 * stop claiming a connection over a link that was gone. But only
 * MQTT_EVENT_CONNECTED ever set it again, and a link that drops and returns with
 * the same address -- a WiFi reconnect inside the IP-lost timer -- leaves the TCP
 * socket intact. esp-mqtt never saw a disconnect, never sent CONNECTED, and the
 * flag stayed false for good: every publish refused, the device silent, while
 * inbound messages still arrived and kept any "am I alive" watchdog satisfied.
 *
 * Keeping the session flag as esp-mqtt reports it and ANDing in the link means
 * a link that returns restores publishing, and a socket that did not survive is
 * reported by esp-mqtt as a disconnect in the usual way.
 */
bool mqtt_manager_is_connected(void)
{
    return s_connected && s_link_up && s_client != NULL;
}

static esp_err_t full_topic(char *out, size_t out_size, const char *suffix)
{
    if (!mqtt_topic_build(out, out_size, s_prefix, NULL, suffix)) {
        return MQTT_MANAGER_ERR_TOPIC_TOO_LONG;
    }
    return ESP_OK;
}

/*
 * Re-subscribe everything.
 *
 * A broker keeps no subscription state for a client that dropped, so this runs on
 * every MQTT_EVENT_CONNECTED and not once at startup. Missing it is the classic
 * MQTT bug: everything works until the first reconnect, after which inbound
 * messages simply stop.
 */
static void resubscribe_all(void)
{
    for (size_t i = 0; i < s_sub_count; i++) {
        int msg_id = esp_mqtt_client_subscribe(s_client, s_subs[i].filter,
                                               s_subs[i].qos);
        if (msg_id < 0) {
            ESP_LOGW(TAG, "subscribing to %s failed", s_subs[i].filter);
        }
    }
    if (s_sub_count) {
        ESP_LOGI(TAG, "re-subscribed %u topic(s)", (unsigned)s_sub_count);
    }
}

static void publish_birth(void)
{
    if (s_lwt_topic[0] == '\0') {
        return;
    }
    /* Retained, and paired with the will the broker holds: together they mean a
     * subscriber learns the device is present or absent without polling, and
     * learns it on connect rather than only on the next change. */
    esp_mqtt_client_publish(s_client, s_lwt_topic, s_cfg.lwt_online,
                            (int)strlen(s_cfg.lwt_online), s_cfg.lwt_qos, true);
}

static void dispatch(esp_mqtt_event_handle_t event)
{
    for (size_t i = 0; i < s_sub_count; i++) {
        if (mqtt_topic_matches(s_subs[i].filter, event->topic,
                               (size_t)event->topic_len)) {
            s_subs[i].cb(event->topic, (size_t)event->topic_len,
                         event->data, (size_t)event->data_len, s_subs[i].ctx);
            /* No break: two handlers may legitimately want the same message,
             * e.g. a specific topic and a "#" tap for diagnostics. */
        }
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to %s", s_cfg.uri);
            s_connected = true;
            publish_birth();
            resubscribe_all();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "disconnected");
            s_connected = false;
            break;

        case MQTT_EVENT_DATA:
            dispatch(event);
            break;

        case MQTT_EVENT_ERROR:
            if (event->error_handle &&
                event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGW(TAG, "transport error: esp_tls 0x%x, stack 0x%x, errno %d",
                         event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_tls_stack_err,
                         event->error_handle->esp_transport_sock_errno);
            }
            break;

        default:
            break;
    }
}

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch ((net_event_id_t)id) {
        case NET_EVENT_LINK_UP:
            s_link_up = true;
            if (s_client && !s_running) {
                if (esp_mqtt_client_start(s_client) == ESP_OK) {
                    s_running = true;
                }
            }
            break;

        case NET_EVENT_LINK_DOWN:
            /*
             * Leave the client running. esp-mqtt reconnects on its own, and
             * esp_mqtt_client_stop() blocks waiting for its task -- which must not
             * happen on the event loop.
             *
             * Leave s_connected alone too: it is the client's to set and clear.
             * Clearing s_link_up is enough for is_connected() to report false,
             * and, unlike clearing s_connected, it is undone by the matching
             * LINK_UP. See mqtt_manager_is_connected().
             */
            s_link_up = false;
            break;

        default:
            break;
    }
}

esp_err_t mqtt_manager_start(const mqtt_manager_config_t *cfg, const char *device_name)
{
    if (!cfg || cfg->uri[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mqtt_topic_needs_device(cfg->topic_prefix) &&
        (!device_name || device_name[0] == '\0')) {
        ESP_LOGE(TAG, "topic_prefix contains $DEVICE$ but no device name was given");
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;

    if (!mqtt_topic_build(s_prefix, sizeof(s_prefix), cfg->topic_prefix,
                          device_name, NULL)) {
        return MQTT_MANAGER_ERR_TOPIC_TOO_LONG;
    }

    s_lwt_topic[0] = '\0';
    if (s_cfg.lwt_suffix[0] != '\0' &&
        full_topic(s_lwt_topic, sizeof(s_lwt_topic), s_cfg.lwt_suffix) != ESP_OK) {
        return MQTT_MANAGER_ERR_TOPIC_TOO_LONG;
    }

    const char *client_id = s_cfg.client_id[0] ? s_cfg.client_id : device_name;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_cfg.uri,
        .credentials.client_id = client_id,
        .session.keepalive = s_cfg.keepalive_s,
        .network.disable_auto_reconnect = false,
    };

    /* esp-mqtt treats an empty username as "no credentials"; passing "" would
     * otherwise offer an empty username to a broker that allows anonymous. */
    if (s_cfg.username[0] != '\0') {
        mqtt_cfg.credentials.username = s_cfg.username;
        mqtt_cfg.credentials.authentication.password = s_cfg.password;
    }

    if (s_lwt_topic[0] != '\0') {
        mqtt_cfg.session.last_will.topic = s_lwt_topic;
        mqtt_cfg.session.last_will.msg = s_cfg.lwt_offline;
        mqtt_cfg.session.last_will.msg_len = (int)strlen(s_cfg.lwt_offline);
        mqtt_cfg.session.last_will.qos = s_cfg.lwt_qos;
        mqtt_cfg.session.last_will.retain = 1;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    err = esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, &on_net_event, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    /* Registering subscriptions before the link exists is normal, so the topics
     * are already built by now; only the connection is missing. If the link came
     * up before this call, connect immediately rather than waiting for a flap. */
    if (s_link_up) {
        if (esp_mqtt_client_start(s_client) == ESP_OK) {
            s_running = true;
        }
    }

    ESP_LOGI(TAG, "publishing under %s", s_prefix);
    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_event_handler_unregister(NET_EVENT, ESP_EVENT_ANY_ID, &on_net_event);

    /* Stopping does not dispatch MQTT_EVENT_DISCONNECTED, so the flag is cleared
     * here or is_connected() keeps claiming a connection that is gone. */
    s_connected = false;

    if (s_running) {
        esp_mqtt_client_stop(s_client);
        s_running = false;
    }
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    return ESP_OK;
}

esp_err_t mqtt_manager_subscribe(const char *topic_suffix, int qos,
                                 mqtt_message_cb_t cb, void *ctx)
{
    if (!topic_suffix || !cb || qos < 0 || qos > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sub_count == MAX_SUBSCRIPTIONS) {
        return ESP_ERR_NO_MEM;
    }

    subscription_t *sub = &s_subs[s_sub_count];
    esp_err_t err = full_topic(sub->filter, sizeof(sub->filter), topic_suffix);
    if (err != ESP_OK) {
        return err;
    }
    sub->qos = qos;
    sub->cb = cb;
    sub->ctx = ctx;
    s_sub_count++;

    /* Registered while already connected: subscribe now rather than making the
     * caller wait for a reconnect. */
    if (mqtt_manager_is_connected()) {
        esp_mqtt_client_subscribe(s_client, sub->filter, qos);
    }

    return ESP_OK;
}

static esp_err_t publish_common(const char *topic_suffix, const void *data,
                                size_t len, int qos, bool retain, bool enqueue)
{
    if (!topic_suffix) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mqtt_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[MAX_TOPIC];
    esp_err_t err = full_topic(topic, sizeof(topic), topic_suffix);
    if (err != ESP_OK) {
        return err;
    }

    int msg_id = enqueue
        ? esp_mqtt_client_enqueue(s_client, topic, (const char *)data, (int)len,
                                  qos, retain, true)
        : esp_mqtt_client_publish(s_client, topic, (const char *)data, (int)len,
                                  qos, retain);

    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_manager_publish(const char *topic_suffix, const void *data,
                               size_t len, int qos, bool retain)
{
    return publish_common(topic_suffix, data, len, qos, retain, false);
}

esp_err_t mqtt_manager_enqueue(const char *topic_suffix, const void *data,
                               size_t len, int qos, bool retain)
{
    return publish_common(topic_suffix, data, len, qos, retain, true);
}
