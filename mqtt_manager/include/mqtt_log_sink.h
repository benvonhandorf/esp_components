#ifndef MQTT_LOG_SINK_H
#define MQTT_LOG_SINK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forwards console and log output to MQTT, so a device's log is readable without
 * a serial cable.
 *
 * Registers a diag sink, so it carries the same text the serial port and the web
 * console see -- including the output of a command typed elsewhere.
 *
 * The sink lives here rather than in diag because it is the transport that owns
 * it: diag depends on nothing, and a project that does not use MQTT should not
 * acquire it by wanting a log.
 */

/* `topic_suffix` is relative to the MQTT topic prefix, e.g. "log".
 * mqtt_manager_start() must have been called first. */
esp_err_t mqtt_log_sink_start(const char *topic_suffix, int qos);
esp_err_t mqtt_log_sink_stop(void);

/* Lines dropped because the device was disconnected, publishing failed, or the
 * sink was re-entered. Worth reporting: a log that quietly loses lines is worse
 * than one that says how many it lost. */
uint32_t mqtt_log_sink_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LOG_SINK_H */
