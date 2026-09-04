#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "cli_web.h"
#include "app_config.h"           /* generated: the top-level walker */
#include "app_config_sections.h"  /* generated: APP_CONFIG_SECTIONS(X) */
#include "config_store.h"
#include "mdns_manager.h"
#include "net_events.h"
#include "ntp_manager.h"
#include "wifi_manager.h"
#include "demo_mqtt_config.h"
#include "demo_net.h"
#include "diag.h"
#include "js2c_error_capture.h"

static int failures;

static void expect(bool cond, const char *what) {
    printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

/* --- js2c: a component generating its own config parser from a schema ------- */

static void check_js2c(void) {
    net_config_t cfg;

    /* Schema defaults are applied for absent optional fields. */
    const char *empty = "{}";
    memset(&cfg, 0xAA, sizeof(cfg));
    js2c_error_capture_reset();
    expect(!demo_net_parse(empty, strlen(empty), &cfg), "empty object parses");
    expect(cfg.channel == 6,    "channel defaulted from schema");
    expect(cfg.enabled == true, "enabled defaulted from schema");
    expect(cfg.ssid[0] == '\0', "ssid defaulted from schema");

    const char *ok = "{\"ssid\":\"lab\",\"channel\":11,\"enabled\":false}";
    js2c_error_capture_reset();
    expect(!demo_net_parse(ok, strlen(ok), &cfg), "populated object parses");
    expect(strcmp(cfg.ssid, "lab") == 0, "ssid parsed");
    expect(cfg.channel == 11,            "channel parsed");

    /* Out-of-range is rejected by generated range checks, with a reason. */
    const char *bad = "{\"channel\":99}";
    js2c_error_capture_reset();
    expect(demo_net_parse(bad, strlen(bad), &cfg), "out-of-range channel rejected");
    expect(js2c_error_capture_get()[0] != '\0',    "rejection carries a reason");
    printf("reason: %s\n", js2c_error_capture_get());
}

/* --- config: one authored schema, sections parsed by their owners ----------- */

/*
 * The aggregate composes each component's own generated type, so there is exactly
 * one definition of each config concept, owned by the component that consumes it.
 */
typedef struct {
    app_config_t        top;
    net_config_t        net;
    demo_mqtt_config_t  mqtt;
} app_config_full_t;

/* One line per section, dispatched by the X-macro the schema generated. Adding a
 * section to the schema without wiring it here fails to compile rather than
 * leaving a silently zeroed struct. */
#define SECTION_PARSER_net  json_parse_net_config_with_len
#define SECTION_PARSER_mqtt json_parse_demo_mqtt_config_with_len

#define PARSE_SECTION(name)                                                     \
    do {                                                                        \
        const app_config_json_ref_t *slice = &out->top.name;                    \
        js2c_error_capture_reset();                                             \
        /* In place: the slice indexes the original buffer, so no section is    \
         * copied out before being parsed. */                                   \
        if (SECTION_PARSER_##name(json + slice->index, slice->length,           \
                                  &out->name)) {                                \
            printf("config: %s: %s\n", #name, js2c_error_capture_get());        \
            return false;                                                       \
        }                                                                       \
    } while (0);

static bool config_parse(app_config_full_t *out, const char *json, size_t len)
{
    memset(out, 0, sizeof(*out));

    js2c_error_capture_reset();
    if (json_parse_app_config_with_len(json, len, &out->top)) {
        printf("config: %s\n", js2c_error_capture_get());
        return false;
    }

    APP_CONFIG_SECTIONS(PARSE_SECTION)
    return true;
}

static void check_config(void)
{
    static const char *const JSON =
        "{\"config_version\":1,\"device_name\":\"bench\","
        "\"net\":{\"ssid\":\"lab\",\"channel\":11},"
        "\"mqtt\":{\"uri\":\"mqtt://broker\"}}";

    app_config_full_t cfg;
    expect(config_parse(&cfg, JSON, strlen(JSON)), "aggregate config parses");
    expect(cfg.top.config_version == 1,               "top-level scalar parsed");
    expect(strcmp(cfg.top.device_name, "bench") == 0, "top-level string parsed");
    expect(strcmp(cfg.net.ssid, "lab") == 0,          "net section parsed by its owner");
    expect(cfg.net.channel == 11,                     "net section value parsed");
    expect(strcmp(cfg.mqtt.uri, "mqtt://broker") == 0, "mqtt section parsed by its owner");
    expect(cfg.mqtt.keepalive_s == 60,                "mqtt default came from its own schema");
    expect(APP_CONFIG_SECTIONS_COUNT == 2,            "the schema declares two sections");

    /* config_store takes paths, so the application picks the filesystem. Nothing
     * is mounted here, so this only checks the guard. */
    const config_store_config_t store = {
        .path = "/res/config.json",
        .override_path = "/sdcard/config.json",
        .max_size = 4096,
    };
    expect(config_store_init(&store) == ESP_OK, "config_store accepts its paths");
    char buf[64];
    expect(config_store_read(buf, sizeof(buf), NULL, NULL) == ESP_ERR_NOT_FOUND,
           "reading with nothing mounted reports NOT_FOUND");
}

/* --- networking: the graph only points down -------------------------------- */

/*
 * Nothing here wires wifi to mdns or to ntp. Each service subscribes to
 * NET_EVENT and starts itself when there is a link, which is why wifi_manager's
 * REQUIRES names neither of them.
 */
static void check_networking(void)
{
    static const mdns_manager_txt_t txt[] = {{"path", "/"}};
    static const mdns_manager_service_t http = {
        .type = "_http", .proto = "_tcp", .port = 80,
        .txt = txt, .txt_count = 1,
    };

    const mdns_manager_config_t mdns_cfg = {
        .hostname = "esp-consumer",
        .instance_name = "Consumer Test",
    };
    expect(mdns_manager_start(&mdns_cfg) == ESP_OK, "mdns_manager starts before any link");
    /* Registration is data, not a weak symbol the application must override. */
    expect(mdns_manager_add_service(&http) == ESP_OK, "a service can be advertised");
    expect(!mdns_manager_is_advertising(), "nothing advertised until the link is up");

    ntp_config_t ntp = {0};
    snprintf(ntp.server, sizeof(ntp.server), "pool.ntp.org");
    snprintf(ntp.timezone, sizeof(ntp.timezone), "UTC0");
    ntp.enabled = true;
    ntp.sync_interval_ms = 3600000;
    expect(ntp_manager_start(&ntp) == ESP_OK, "ntp_manager starts before any link");
    expect(!ntp_manager_is_synced(), "clock not claimed synced before a sync");

    expect(ntp_manager_start(&ntp) == ESP_ERR_INVALID_STATE, "starting twice is refused");

    /* The event base is what decouples them; posting is how a link layer other
     * than WiFi would drive the same services. */
    expect(net_events_post(NET_EVENT_LINK_DOWN, NULL, 0) == ESP_OK,
           "link events can be posted");
    expect(strcmp(net_events_name(NET_EVENT_LINK_UP), "LINK_UP") == 0,
           "events have readable names");
}

/* --- cli: a component shipping its own command group ----------------------- */

static int cmd_demo_show(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Commands never call printf(): diag_printf() reaches every interface. */
    diag_printf("demo: ok\n");
    return 0;
}

static const cli_command_t demo_cmds[] = {
    {"show", NULL, "Print a line through the output fan-out", cmd_demo_show},
};

static const cli_group_t demo_group = {
    .name = "demo-net",
    .help = "Demo component commands",
    .commands = demo_cmds,
    .command_count = sizeof(demo_cmds) / sizeof(demo_cmds[0]),
};

void app_main(void)
{
    check_js2c();
    check_config();
    check_networking();

    ESP_ERROR_CHECK(diag_init(NULL));
    ESP_ERROR_CHECK(cli_register_group(&demo_group));

    printf(failures ? "\nCONSUMER TEST FAILED (%d)\n" : "\nCONSUMER TEST PASSED (%d)\n",
           failures);

    /* Brings up the shell on the serial console. Registering groups first means
     * the banner and tab completion see them. */
    ESP_ERROR_CHECK(cli_start(NULL));

    /*
     * cli_web is not started here: it needs an IP address first, and this test
     * never joins a network. Referencing it is enough to prove it links, which
     * is what the build is checking.
     */
    (void)cli_web_start;
    (void)cli_web_stop;
    (void)cli_web_server;
}
