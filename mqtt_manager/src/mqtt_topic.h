#ifndef MQTT_TOPIC_H
#define MQTT_TOPIC_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Topic string handling, kept apart from the client so it can be tested on the
 * host. Both of these are easy to get subtly wrong and impossible to see going
 * wrong on a device: a filter that never matches simply looks like a broker that
 * never publishes.
 */

/*
 * Match a received topic against an MQTT topic filter.
 *
 * Implements the wildcard rules from the MQTT specification:
 *   +   matches exactly one level
 *   #   matches this level and all below it, and must be the last character
 *
 * `topic` need not be NUL-terminated; `topic_len` bounds it, because that is how
 * esp-mqtt hands it over.
 *
 * Subscribing to a wildcard and then dispatching only on an exact string compare
 * would deliver nothing while looking entirely correct, which is why this exists
 * rather than a strncmp at the call site.
 */
bool mqtt_topic_matches(const char *filter, const char *topic, size_t topic_len);

/*
 * Build a full topic from a prefix and a suffix, substituting the device name
 * for every occurrence of "$DEVICE$" in the prefix.
 *
 * The substitution is what lets one configuration file serve a fleet: a prefix of
 * "sensor/$DEVICE$" becomes "sensor/greenhouse-01" on one device and
 * "sensor/greenhouse-02" on the next, with no per-device config.
 *
 * A single "/" is inserted between prefix and suffix if neither supplies one, and
 * a doubled one is avoided if both do. Returns false if the result would not fit.
 */
bool mqtt_topic_build(char *out, size_t out_size, const char *prefix,
                      const char *device_name, const char *suffix);

/* Whether `prefix` contains "$DEVICE$", and so needs a device name to be
 * meaningful. Substituting an empty name would silently collapse a topic level:
 * "sensor/$DEVICE$" would become "sensor/", which joins to "sensor/status" --
 * a different topic than intended, and a valid-looking one. */
bool mqtt_topic_needs_device(const char *prefix);

#endif /* MQTT_TOPIC_H */
