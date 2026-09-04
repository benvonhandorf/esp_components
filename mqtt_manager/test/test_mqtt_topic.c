/*
 * Host test for MQTT topic matching and topic building.
 *
 * Both are pure string logic, and both fail invisibly on a device: a filter that
 * never matches looks exactly like a broker that never publishes, and a topic
 * built wrong looks exactly like a subscriber that is not listening.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "mqtt_topic.h"

static int failures;

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

static void match(const char *filter, const char *topic, bool want)
{
    bool got = mqtt_topic_matches(filter, topic, strlen(topic));
    char label[160];
    snprintf(label, sizeof(label), "'%s' %s '%s'", filter,
             want ? "matches" : "does not match", topic);
    expect(got == want, label);
}

static void built(const char *prefix, const char *device, const char *suffix,
                  const char *want)
{
    char out[128];
    bool ok = mqtt_topic_build(out, sizeof(out), prefix, device, suffix);
    char label[220];
    snprintf(label, sizeof(label), "'%s' + '%s' -> '%s'", prefix, suffix, want);
    expect(ok && strcmp(out, want) == 0, label);
    if (!ok || strcmp(out, want) != 0) {
        printf("       got '%s'\n", out);
    }
}

static void test_exact(void)
{
    match("sensor/a/status", "sensor/a/status", true);
    match("sensor/a/status", "sensor/a/statu",  false);
    match("sensor/a/status", "sensor/a/status2", false);
    match("sensor/a",        "sensor/a/status", false);
    match("",                "",                true);
}

static void test_single_level_wildcard(void)
{
    match("sensor/+/status", "sensor/a/status",   true);
    match("sensor/+/status", "sensor/bcd/status", true);
    match("sensor/+/status", "sensor//status",    true);   /* empty level is a level */
    match("sensor/+/status", "sensor/a/b/status", false);  /* + is one level only */
    match("sensor/+/status", "sensor/status",     false);
    match("+",               "sensor",            true);
    match("+/status",        "sensor/status",     true);
    match("sensor/+",        "sensor/a",          true);
    match("sensor/+",        "sensor/a/b",        false);
    /* A '+' must occupy a whole level, so this is not a prefix match. */
    match("sensor/a+/x",     "sensor/ab/x",       false);
}

static void test_multi_level_wildcard(void)
{
    match("sensor/#",   "sensor/a",       true);
    match("sensor/#",   "sensor/a/b/c",   true);
    /* Per the spec, "sensor/#" also matches the parent level itself. */
    match("sensor/#",   "sensor",         true);
    match("sensor/#",   "sensors/a",      false);
    match("#",          "anything/at/all", true);
    match("sensor/+/#", "sensor/a/b/c",   true);
    /* '#' is only valid as the last character. */
    match("sensor/#/x", "sensor/a/x",     false);
}

static void test_not_nul_terminated(void)
{
    /* esp-mqtt hands over a length, not a terminator. */
    const char buffer[] = "sensor/a/statusGARBAGE";
    expect(mqtt_topic_matches("sensor/a/status", buffer, 15),
           "matching respects the given length, not a terminator");
    expect(!mqtt_topic_matches("sensor/a/status", buffer, 22),
           "and does not match when the length includes trailing bytes");
}

static void test_build(void)
{
    built("sensor/dev", NULL, "status", "sensor/dev/status");
    built("sensor/dev/", NULL, "status", "sensor/dev/status");     /* no doubling */
    built("sensor/dev", NULL, "/status", "sensor/dev/status");
    built("sensor/dev/", NULL, "/status", "sensor/dev/status");
    built("sensor/dev", NULL, "", "sensor/dev");                   /* suffix optional */

    /* One config file, a fleet of devices. */
    built("sensor/$DEVICE$", "greenhouse-01", "status", "sensor/greenhouse-01/status");
    built("$DEVICE$", "dev", "status", "dev/status");
    built("a/$DEVICE$/b/$DEVICE$", "x", "s", "a/x/b/x/s");
    /* An empty device name collapses the level -- "sensor/$DEVICE$" becomes
     * "sensor/", which joins to "sensor/status". That is a configuration error,
     * so mqtt_topic_needs_device() lets the caller refuse it up front rather
     * than publishing to a plausible-looking wrong topic. */
    built("sensor/$DEVICE$", "", "status", "sensor/status");
    expect(mqtt_topic_needs_device("sensor/$DEVICE$"), "a prefix with $DEVICE$ needs a name");
    expect(!mqtt_topic_needs_device("sensor/plain"), "a plain prefix does not");
    expect(!mqtt_topic_needs_device(NULL), "NULL needs nothing");
    built("sensor/plain", "unused", "status", "sensor/plain/status");
}

static void test_build_refuses_to_truncate(void)
{
    char small[8];
    /* A truncated topic is a topic that silently goes somewhere else, so this
     * reports failure rather than publishing to the wrong place. */
    expect(!mqtt_topic_build(small, sizeof(small), "sensor/device", NULL, "status"),
           "building refuses to truncate");
    expect(mqtt_topic_build(small, sizeof(small), "a", NULL, "b"),
           "a topic that fits still builds");
}

int main(void)
{
    test_exact();
    test_single_level_wildcard();
    test_multi_level_wildcard();
    test_not_nul_terminated();
    test_build();
    test_build_refuses_to_truncate();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
