# mqtt_manager

MQTT client with a topic-handler registry, last-will/birth messages, and an optional log
sink. It knows nothing about what the device does.

```c
mqtt_manager_start(&cfg, "greenhouse-01");   /* at boot; connects when the link is up */

mqtt_manager_subscribe("relays", 0, on_relay_message, NULL);
mqtt_manager_publish("status", payload, len, 0, false);
```

Topics are given as **suffixes under the configured prefix**, so a project's whole topic
tree moves by editing one configuration value.

## Nothing application-specific is in here

Its predecessor `#include`d the application's `relay_control.h` and `system_status.h`,
hardcoded a relay topic, parsed relay JSON in its own event handler, and took a relay
command queue in `mqtt_manager_init()`. Its header guard was `SOLAR_MQTT_CLIENT_H`. It
could only ever be used by the one project it came from.

Everything domain-specific now enters through `mqtt_manager_subscribe()` and
`mqtt_manager_publish()`. The **status message belongs to the application** — this
component publishes bytes and has no opinion about their shape.

## Subscriptions are re-applied on every reconnect

A broker keeps no subscription state for a client that dropped. Missing this is the
classic MQTT bug: everything works until the first reconnect, after which inbound messages
simply stop. The registry is re-subscribed on `MQTT_EVENT_CONNECTED`.

Filters are matched **properly**, with `+` and `#` per the specification. Subscribing to a
wildcard and dispatching on an exact string compare would deliver nothing while looking
entirely correct.

A message is delivered to *every* matching handler, not just the first: a specific topic
and a `#` tap for diagnostics may both legitimately want it.

## Last will and birth

Set `lwt_suffix` (empty disables it). The broker publishes `lwt_offline` retained when the
client stops responding; the client publishes `lwt_online` retained on connect. Together
they let a subscriber know whether the device is present without polling, and learn it on
connect rather than only at the next change.

## `$DEVICE$`

`topic_prefix` substitutes `$DEVICE$` with the device name passed to
`mqtt_manager_start()`, so one configuration file serves a fleet: `sensor/$DEVICE$` becomes
`sensor/greenhouse-01` on one unit and `sensor/greenhouse-02` on the next.

Starting with `$DEVICE$` in the prefix and no device name is **refused**: substituting
nothing collapses a topic level, turning `sensor/$DEVICE$/status` into `sensor/status` —
a different topic, and a plausible-looking one.

(The predecessor's schema documented this substitution; it was never implemented.)

## The log sink

```c
mqtt_log_sink_start("log", 0);
```

Registers a [`diag`](../diag/) sink, so it carries the same text the serial port and web
console see. Two hazards it has to avoid, both of which are silent when got wrong:

- **Re-entry.** Publishing logs on failure; that log goes to diag; diag calls the sink;
  it publishes again. Guarded per-task, so output from an unrelated task is not dropped
  merely because another task is inside a publish.
- **Deadlock.** diag holds its output lock across the sink call, and
  `esp_mqtt_client_publish()` blocks until the client task takes the message — waiting on
  a task that may itself need to log. The sink uses `mqtt_manager_enqueue()`, which stores
  to the outbox and returns.

Lines dropped while disconnected are counted (`mqtt_log_sink_dropped()`) rather than lost
silently.

## Known bugs

- **The "link is already up" shortcut in `mqtt_manager_start()` can never fire on a first
  start.** `s_link_up` is set only by the `NET_EVENT` handler, and that handler is
  registered by `start()` itself a few lines earlier — so on the first call the flag is
  still false however long the network has been up, and the client is not started. It then
  waits for the *next* `NET_EVENT_LINK_UP`, which on a device that is already connected
  means the next time the link drops and returns. The symptom is a device that is plainly
  on the network and never reaches the broker.

  It works today only because callers start MQTT before the network — the order every
  example here uses. A caller that starts MQTT lazily, after a link exists, hits it.

  The flag is also stale across a `stop()`/`start()` pair: `stop()` unregisters the
  handler but leaves `s_link_up` set, so the second `start()` may believe in a link that
  went away while it was stopped. Both want the same fix — ask the network whether an
  interface has an address rather than inferring it from an event this component may have
  been absent for.

## Tests

```sh
make -C test
```

Topic matching and topic building are pure string logic and both fail invisibly on a
device, so they are tested on the host — a filter that never matches looks exactly like a
broker that never publishes.
