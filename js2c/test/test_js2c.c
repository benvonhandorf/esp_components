/*
 * Host test for the js2c component: schema defaults, range rejection, error
 * capture, cross-file $refs, and the "raw" section-slice mode the project-level
 * config walker is generated from.
 *
 * These parsers have no ESP-IDF dependency, so this builds with plain gcc and
 * compiles the *real* generated sources -- not a hand-copy of them.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "net_config.h"
#include "app_config.h"
#include "js2c_error_capture.h"

static int failures;

static void expect(bool cond, const char *what) {
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

static void test_defaults_come_from_the_schema(void) {
    net_config_t c;
    memset(&c, 0xAA, sizeof(c));
    js2c_error_capture_reset();
    expect(!json_parse_net_config_with_len("{}", 2, &c), "empty object parses");
    expect(c.channel == 6,    "channel default applied");
    expect(c.enabled == true, "enabled default applied");
    expect(c.ssid[0] == '\0', "ssid default applied");
    /* maxLength is ABI: 32 characters plus the terminator. */
    expect(sizeof(c.ssid) == 33, "maxLength fixes the buffer size");
    /* js2cType overrides the default uint64_t for integers. */
    expect(sizeof(c.channel) == 1, "js2cType narrows the integer");
}

static void test_values_parse(void) {
    net_config_t c;
    const char *s = "{\"ssid\":\"lab\",\"channel\":11,\"enabled\":false}";
    js2c_error_capture_reset();
    expect(!json_parse_net_config_with_len(s, strlen(s), &c), "populated object parses");
    expect(strcmp(c.ssid, "lab") == 0, "string parsed");
    expect(c.channel == 11,            "integer parsed");
    expect(c.enabled == false,         "boolean parsed");
}

static void test_range_and_unknown_are_rejected_with_a_reason(void) {
    net_config_t c;
    const char *over = "{\"channel\":99}";
    js2c_error_capture_reset();
    expect(json_parse_net_config_with_len(over, strlen(over), &c), "out-of-range rejected");
    expect(js2c_error_capture_get()[0] != '\0', "range failure carries a reason");
    printf("       reason: %s\n", js2c_error_capture_get());

    const char *unknown = "{\"nope\":1}";
    js2c_error_capture_reset();
    expect(json_parse_net_config_with_len(unknown, strlen(unknown), &c),
           "unknown field rejected (additionalProperties: false)");
    expect(js2c_error_capture_get()[0] != '\0', "unknown field carries a reason");
    printf("       reason: %s\n", js2c_error_capture_get());
}

static void test_cross_file_ref_and_raw_sections(void) {
    app_config_t a;
    const char *s = "{\"config_version\":1,\"broker_port\":8883,"
                    "\"net\":{\"ssid\":\"lab\",\"channel\":3}}";
    js2c_error_capture_reset();
    expect(!json_parse_app_config_with_len(s, strlen(s), &a), "aggregate parses");
    /* The cross-file $ref resolved and kept the referenced schema's own $id. */
    expect(a.broker_port == 8883, "cross-file $ref parsed");
    expect(sizeof(a.broker_port) == 2, "cross-file $ref kept its js2cType");

    /* A "raw" field is a slice of the input, which is how the top-level walker
     * hands each section to its owning component's parser with no copy. */
    expect(a.net.length > 0, "raw section recorded a length");
    net_config_t n;
    js2c_error_capture_reset();
    expect(!json_parse_net_config_with_len(s + a.net.index, a.net.length, &n),
           "raw slice re-parses in place as the section's own type");
    expect(strcmp(n.ssid, "lab") == 0, "section content survived the slice");
    expect(n.channel == 3,             "section content survived the slice (int)");
    expect(n.enabled == true,          "section default still applied inside slice");

    /* A missing required section is a generated error, not a silent zero. */
    const char *missing = "{\"config_version\":1}";
    js2c_error_capture_reset();
    expect(json_parse_app_config_with_len(missing, strlen(missing), &a),
           "missing required section rejected");
    printf("       reason: %s\n", js2c_error_capture_get());

    /* config_version is gated by the schema's own range check. */
    const char *badver = "{\"config_version\":2,\"net\":{}}";
    js2c_error_capture_reset();
    expect(json_parse_app_config_with_len(badver, strlen(badver), &a),
           "unsupported config_version rejected");
    printf("       reason: %s\n", js2c_error_capture_get());
}

int main(void) {
    test_defaults_come_from_the_schema();
    test_values_parse();
    test_range_and_unknown_are_rejected_with_a_reason();
    test_cross_file_ref_and_raw_sections();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
