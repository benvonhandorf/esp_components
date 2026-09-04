/*
 * Host test for the sections pipeline: js2c_sections.py derives a top-level
 * walker from the authored config schema, and each section is handed as a slice
 * to the parser belonging to the component that owns it.
 *
 * This is the whole point of the arrangement, so it is tested end to end against
 * real generated code: the aggregate schema, the derived sections schema, and
 * the two component fragments all go through the real generator.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "app_config.h"           /* generated from the derived sections schema */
#include "app_config_sections.h"  /* generated: APP_CONFIG_SECTIONS(X) */
#include "wifi_config.h"          /* generated from wifi_manager's fragment */
#include "mqtt_config.h"          /* generated from mqtt_manager's fragment */
#include "js2c_error_capture.h"

static int failures;

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

/*
 * What a project's config_reader looks like.
 *
 * The aggregate struct composes each component's own generated type, so there is
 * exactly one definition of each config concept, owned by the component that
 * consumes it.
 */
typedef struct {
    app_config_t  top;
    wifi_config_t wifi;
    mqtt_config_t mqtt;
} app_config_full_t;

/* One line per section, named by the X-macro so a section added to the schema
 * cannot be silently left undispatched -- it expands to an X() naming a parser
 * and a struct member that do not exist yet, and the build fails. */
#define SECTION_PARSER_wifi json_parse_wifi_config_with_len
#define SECTION_PARSER_mqtt json_parse_mqtt_config_with_len

#define PARSE_SECTION(name)                                                        \
    do {                                                                           \
        const app_config_json_ref_t *slice = &out->top.name;                       \
        js2c_error_capture_reset();                                                \
        /* In place: the slice indexes the original buffer, so no section is       \
         * ever copied out before being parsed. */                                 \
        if (SECTION_PARSER_##name(json + slice->index, slice->length,              \
                                  &out->name)) {                                   \
            snprintf(err, err_len, "%s: %s", #name, js2c_error_capture_get());     \
            return false;                                                          \
        }                                                                          \
    } while (0);

static bool config_parse(app_config_full_t *out, const char *json, size_t len,
                         char *err, size_t err_len)
{
    memset(out, 0, sizeof(*out));
    err[0] = '\0';

    js2c_error_capture_reset();
    if (json_parse_app_config_with_len(json, len, &out->top)) {
        snprintf(err, err_len, "%s", js2c_error_capture_get());
        return false;
    }

    APP_CONFIG_SECTIONS(PARSE_SECTION)
    return true;
}

static const char *const GOOD_CONFIG =
    "{"
    "  \"config_version\": 1,"
    "  \"device_name\": \"bench\","
    "  \"wifi\": { \"ssid\": \"lab\", \"password\": \"secret\" },"
    "  \"mqtt\": { \"uri\": \"mqtt://broker\", \"keepalive_s\": 30 }"
    "}";

static void test_sections_are_parsed_by_their_owners(void)
{
    app_config_full_t cfg;
    char err[192];

    expect(config_parse(&cfg, GOOD_CONFIG, strlen(GOOD_CONFIG), err, sizeof(err)),
           "a complete config parses");
    expect(cfg.top.config_version == 1,            "top-level scalar parsed");
    expect(strcmp(cfg.top.device_name, "bench") == 0, "top-level string parsed");
    expect(strcmp(cfg.wifi.ssid, "lab") == 0,      "wifi section parsed by wifi's parser");
    expect(strcmp(cfg.mqtt.uri, "mqtt://broker") == 0, "mqtt section parsed by mqtt's parser");
    expect(cfg.mqtt.keepalive_s == 30,             "section integer parsed");

    /* Defaults come from each component's own fragment, not from C. */
    expect(strcmp(cfg.wifi.hostname, "esp-device") == 0,
           "an absent field takes the default from its own component's schema");
}

static void test_slices_point_into_the_original_buffer(void)
{
    app_config_full_t cfg;
    char err[192];
    config_parse(&cfg, GOOD_CONFIG, strlen(GOOD_CONFIG), err, sizeof(err));

    /* The walker records offsets, so a section is parsed where it lies. */
    expect(cfg.top.wifi.length > 0, "wifi slice has a length");
    expect(GOOD_CONFIG[cfg.top.wifi.index] == '{', "wifi slice starts at its object");
    expect(memmem(GOOD_CONFIG + cfg.top.wifi.index, cfg.top.wifi.length, "lab", 3) != NULL,
           "wifi slice spans its own content");
    expect(memmem(GOOD_CONFIG + cfg.top.wifi.index, cfg.top.wifi.length, "broker", 6) == NULL,
           "wifi slice does not run into the mqtt section");
}

static void test_all_defaults(void)
{
    app_config_full_t cfg;
    char err[192];
    /* An empty object per section means "all defaults" -- a section is required
     * because a raw field cannot carry one. */
    const char *json = "{\"config_version\":1,\"wifi\":{},\"mqtt\":{}}";

    expect(config_parse(&cfg, json, strlen(json), err, sizeof(err)),
           "empty sections parse");
    expect(cfg.mqtt.keepalive_s == 60, "section default applied through the slice");
    expect(strcmp(cfg.wifi.hostname, "esp-device") == 0, "wifi default applied");
}

static void test_failures_name_the_section(void)
{
    app_config_full_t cfg;
    char err[192];

    const char *bad_section = "{\"config_version\":1,\"wifi\":{\"ssid\":\"x\"},"
                              "\"mqtt\":{\"keepalive_s\":9999}}";
    expect(!config_parse(&cfg, bad_section, strlen(bad_section), err, sizeof(err)),
           "an out-of-range value inside a section is rejected");
    expect(strncmp(err, "mqtt:", 5) == 0, "the failing section is named");
    printf("       %s\n", err);

    const char *missing = "{\"config_version\":1,\"wifi\":{}}";
    expect(!config_parse(&cfg, missing, strlen(missing), err, sizeof(err)),
           "a missing section is rejected, not silently zeroed");
    printf("       %s\n", err);

    const char *bad_version = "{\"config_version\":2,\"wifi\":{},\"mqtt\":{}}";
    expect(!config_parse(&cfg, bad_version, strlen(bad_version), err, sizeof(err)),
           "an unsupported config_version is rejected before any section is parsed");
    printf("       %s\n", err);

    /* Forward compatibility: a section this firmware does not know about is
     * skipped rather than failing the whole file. */
    const char *unknown = "{\"config_version\":1,\"wifi\":{},\"mqtt\":{},"
                          "\"relays\":[{\"id\":1}]}";
    expect(config_parse(&cfg, unknown, strlen(unknown), err, sizeof(err)),
           "an unknown top-level section is skipped, not fatal");
}

static void test_token_budget_covers_a_full_document(void)
{
    app_config_full_t cfg;
    char err[192];
    /* Every field of every section populated: the tokenizer must hold the whole
     * document even though each section costs the walker one token. */
    const char *full =
        "{\"config_version\":1,\"device_name\":\"a-fairly-long-device-name\","
        "\"wifi\":{\"ssid\":\"network\",\"password\":\"passphrase\",\"hostname\":\"host\"},"
        "\"mqtt\":{\"uri\":\"mqtt://broker.example.com:1883\","
        "\"topic_root\":\"sensor/device/001\",\"keepalive_s\":120}}";
    expect(config_parse(&cfg, full, strlen(full), err, sizeof(err)),
           "a fully populated document fits the computed token budget");
    expect(cfg.mqtt.keepalive_s == 120, "and parses correctly");
}

int main(void)
{
    test_sections_are_parsed_by_their_owners();
    test_slices_point_into_the_original_buffer();
    test_all_defaults();
    test_failures_name_the_section();
    test_token_budget_covers_a_full_document();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
