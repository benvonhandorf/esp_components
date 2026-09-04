#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "mqtt_manager_config.h"   /* generated from the schema */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MQTT client with a topic-handler registry.
 *
 * Start it at boot, before there is any connectivity: it subscribes to
 * NET_EVENT_LINK_UP and connects when there is a network, so no caller has to
 * sequence it against the link coming up.
 *
 * It knows nothing about what the device does. Its predecessor included the
 * application's relay-control and system-status headers, hardcoded a relay topic
 * and took a relay command queue in its init, so it could only ever be used by
 * the one project it came from. Everything domain-specific now enters through
 * mqtt_manager_subscribe() and mqtt_manager_publish(), and the status message
 * belongs to the application.
 *
 * Topics are given as suffixes under the configured prefix, so a project's whole
 * topic tree moves by editing one configuration value.
 */

#define MQTT_MANAGER_ERR_BASE 0x34000
/* The topic did not fit. Distinct from ESP_ERR_INVALID_SIZE so a caller can tell
 * a too-long topic from a too-long payload. */
#define MQTT_MANAGER_ERR_TOPIC_TOO_LONG (MQTT_MANAGER_ERR_BASE + 1)

/*
 * Called on the MQTT client's task when a message arrives on a matching topic.
 *
 * Neither `topic` nor `data` is NUL-terminated; both are bounded by their length,
 * which is how esp-mqtt delivers them. Do not block here -- the client task is
 * also what services keepalives, so a slow handler drops the connection. Copy
 * what you need and hand it to your own task.
 */
typedef void (*mqtt_message_cb_t)(const char *topic, size_t topic_len,
                                  const char *data, size_t data_len, void *ctx);

/*
 * Configure and start. `device_name` is substituted for "$DEVICE$" in the topic
 * prefix and used as the client id when the config leaves it empty.
 *
 * ESP_ERR_INVALID_ARG if the prefix contains "$DEVICE$" and no device name is
 * given -- substituting nothing would collapse a topic level and publish to a
 * plausible-looking wrong topic.
 */
esp_err_t mqtt_manager_start(const mqtt_manager_config_t *cfg, const char *device_name);
esp_err_t mqtt_manager_stop(void);

/* True only when the broker connection is actually up. */
bool mqtt_manager_is_connected(void);

/*
 * Register interest in a topic under the prefix.
 *
 * `topic_suffix` is an MQTT topic filter, so "+" and "#" work and are matched
 * properly when dispatching -- subscribing to a wildcard and dispatching on an
 * exact compare would deliver nothing while looking correct.
 *
 * May be called before the connection exists, and before or after start(): the
 * table is re-subscribed on every reconnect, because a broker keeps no
 * subscription state for a client that dropped.
 *
 * ESP_ERR_NO_MEM if the table is full (CONFIG_MQTT_MANAGER_MAX_SUBSCRIPTIONS).
 */
esp_err_t mqtt_manager_subscribe(const char *topic_suffix, int qos,
                                 mqtt_message_cb_t cb, void *ctx);

/*
 * Publish under the prefix. Returns ESP_ERR_INVALID_STATE when not connected;
 * messages are not queued for a broker that is not there.
 */
esp_err_t mqtt_manager_publish(const char *topic_suffix, const void *data,
                               size_t len, int qos, bool retain);

/*
 * Publish without waiting for the client task to take the message.
 *
 * For callers that must not block -- a log sink, an ISR-adjacent path. The
 * message is stored in the outbox and sent when the client gets to it.
 */
esp_err_t mqtt_manager_enqueue(const char *topic_suffix, const void *data,
                               size_t len, int qos, bool retain);

/* The resolved topic prefix, with "$DEVICE$" substituted. Useful for reporting
 * what the device is actually publishing under. NULL before start(). */
const char *mqtt_manager_topic_prefix(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_MANAGER_H */
