#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"

#include "cli.h"
#include "cli_web.h"
#include "app_config.h"           /* generated: the top-level walker */
#include "app_config_sections.h"  /* generated: APP_CONFIG_SECTIONS(X) */
#include "config_store.h"
#include "http_server.h"
#include "aw9523b.h"
#include "ina219.h"
#include "ina226.h"
#include "ina239.h"
#include "int_dispatch.h"
#include "lm75bdp.h"
#include "pi4ioe5v6408.h"
#include "rx8130ce.h"
#include "mdns_manager.h"
#include "ota.h"
#include "ota_http.h"
#include "mqtt_log_sink.h"
#include "mqtt_manager.h"
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

    /* Scanning belongs to whoever owns the SCAN_DONE handler: a caller that
     * called esp_wifi_scan_start() itself would race this component and lose.
     * Uninitialised, it refuses rather than touching a driver that is not up. */
    wifi_ap_record_t records[4];
    uint16_t found = 1;
    expect(wifi_manager_scan(records, 4, &found) == ESP_ERR_INVALID_STATE,
           "wifi_manager_scan is refused before init");
    expect(found == 0, "a refused scan reports no records");
    expect(wifi_manager_scan(NULL, 4, &found) == ESP_ERR_INVALID_ARG,
           "wifi_manager_scan rejects a NULL record array");

    ntp_manager_config_t ntp = {0};
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

/* --- mqtt: nothing about it knows what this device does --------------------- */

static int relay_messages;

static void on_relay_message(const char *topic, size_t topic_len,
                             const char *data, size_t data_len, void *ctx)
{
    (void)topic; (void)topic_len; (void)data; (void)data_len; (void)ctx;
    relay_messages++;
}

static void check_mqtt(void)
{
    mqtt_manager_config_t cfg = {0};
    snprintf(cfg.uri, sizeof(cfg.uri), "mqtt://broker.invalid:1883");
    snprintf(cfg.topic_prefix, sizeof(cfg.topic_prefix), "sensor/$DEVICE$");
    snprintf(cfg.lwt_suffix, sizeof(cfg.lwt_suffix), "online");
    snprintf(cfg.lwt_online, sizeof(cfg.lwt_online), "1");
    snprintf(cfg.lwt_offline, sizeof(cfg.lwt_offline), "0");
    cfg.keepalive_s = 120;
    cfg.lwt_qos = 1;

    /* $DEVICE$ with no device name would collapse a topic level and publish to a
     * plausible-looking wrong topic, so it is refused rather than substituted. */
    expect(mqtt_manager_start(&cfg, NULL) == ESP_ERR_INVALID_ARG,
           "$DEVICE$ without a device name is refused");

    expect(mqtt_manager_start(&cfg, "unit-01") == ESP_OK, "mqtt_manager starts");
    expect(strcmp(mqtt_manager_topic_prefix(), "sensor/unit-01") == 0,
           "$DEVICE$ substituted into the topic prefix");
    expect(!mqtt_manager_is_connected(), "not connected without a broker");

    /* Domain knowledge enters through the registry, not through this component's
     * headers: the predecessor included relay_control.h and hardcoded the topic. */
    expect(mqtt_manager_subscribe("relays", 0, on_relay_message, NULL) == ESP_OK,
           "a handler can be registered before any connection");
    expect(mqtt_manager_subscribe("cmd/+", 1, on_relay_message, NULL) == ESP_OK,
           "a wildcard filter can be registered");

    /* Nothing is queued for a broker that is not there. */
    expect(mqtt_manager_publish("status", "{}", 2, 0, false) == ESP_ERR_INVALID_STATE,
           "publishing while disconnected reports it");

    expect(mqtt_log_sink_start("log", 0) == ESP_OK, "the log sink attaches to diag");
    diag_printf("this line goes nowhere while disconnected\n");
    expect(mqtt_log_sink_dropped() > 0, "and is counted as dropped, not lost silently");
    expect(mqtt_log_sink_stop() == ESP_OK, "the log sink detaches");

    expect(mqtt_manager_stop() == ESP_OK, "mqtt_manager stops");
}

/* --- http + ota: routes are data, and one server serves everything --------- */

static esp_err_t handle_status(httpd_req_t *req)
{
    /* Whatever the route was registered with, without the component having to
     * hand out a global. */
    const char *what = (const char *)http_server_route_ctx(req);
    return httpd_resp_sendstr(req, what ? what : "{}");
}

static void check_http_and_ota(void)
{
    http_server_config_t cfg = {0};
    cfg.port = 8080;
    cfg.require_auth = true;
    cfg.max_open_sockets = 7;
    snprintf(cfg.username, sizeof(cfg.username), "admin");
    snprintf(cfg.realm, sizeof(cfg.realm), "Consumer Test");

    /* A management interface that can reflash the device must not come up with
     * credentials that ship in the source, so this is refused rather than
     * defaulted -- and refused rather than silently disabling authentication. */
    expect(http_server_start(&cfg) == HTTP_SERVER_ERR_NO_PASSWORD,
           "authentication without a password is refused");

    snprintf(cfg.password, sizeof(cfg.password), "not-the-default");
    expect(http_server_start(&cfg) == ESP_OK, "http_server starts once a password is set");

    /* And listens straight away, with no link and no address. httpd binds
     * INADDR_ANY, so the socket is not tied to an interface: it cannot be
     * reached until one exists, and needs no restart when one changes. What that
     * buys is the handle -- valid from here on, which is what lets cli_web share
     * this server instead of opening a second listener on the same port. */
    expect(http_server_is_running(), "and listens immediately, before any link");
    expect(http_server_handle() != NULL, "so the handle is available to share");

    static const char STATUS[] = "{\"ok\":true}";
    static const http_route_t routes[] = {
        {"/api/status", HTTP_GET, handle_status, (void *)STATUS, false},
    };
    expect(http_server_add_routes(routes, 1) == ESP_OK,
           "routes register as data, not through a weak symbol");

    /* The OTA route is authenticated by the server, not by its own check. */
    expect(ota_http_register("/api/ota") == ESP_OK, "the OTA route registers");

    /* This build has a single app partition, which is exactly the condition the
     * template's A/B layout exists to avoid -- and it is reported as its own
     * stage rather than a generic failure. */
    ota_report_t report;
    ota_session_t *session = NULL;
    esp_err_t err = ota_session_begin(NULL, &session, &report);
    expect(err == ESP_ERR_NOT_FOUND, "OTA without a second app slot is refused");
    expect(report.failed_stage == OTA_STAGE_NO_PARTITION, "and names the stage");
    printf("ota stage: %s\n", ota_stage_name(report.failed_stage));

    expect(!ota_pending_verify(), "a normally-booted image awaits no confirmation");

    expect(http_server_stop() == ESP_OK, "http_server stops");
}

/* --- drivers: a handle exists before the bus does --------------------------- */

static void check_drivers(void)
{
    /* The property that makes a driver reusable: it can be created before there
     * is anything to talk to, so a caller may state how a board is wired before
     * it is powered. */
    ina226_config_t cfg = {
        .dev = NULL,
        .shunt_ohms = 0.01f,
        .max_current_a = 32.768f,
    };
    ina226_handle_t ina = NULL;
    ina226_report_t report;

    expect(ina226_create(&cfg, &ina, &report) == ESP_OK, "ina226 handle created with no bus");
    expect(report.calibration == 512, "calibration derived from the shunt, not hardcoded");
    expect(report.current_lsb_a > 0.0f, "and the resolution is reported");

    /* Every call needing the bus refuses, by name, rather than faulting. */
    ina226_reading_t reading;
    expect(ina226_read(ina, &reading) == ESP_ERR_INVALID_STATE,
           "reading without a device reports INVALID_STATE");
    expect(ina226_clear_alert(ina) == ESP_ERR_INVALID_STATE,
           "clearing the alert without a device reports INVALID_STATE");

    /* A shunt and range that cannot be represented is refused up front. */
    ina226_config_t bad = {.shunt_ohms = 0.0001f, .max_current_a = 0.1f};
    ina226_handle_t unused = NULL;
    expect(ina226_create(&bad, &unused, &report) == ESP_ERR_INA226_BAD_RANGE,
           "an unrepresentable range is refused");
    expect(report.failed_stage == INA226_STAGE_RANGE, "and names the stage");

    ina226_delete(ina);

    /* Every converted driver has the same shape, so the same property holds for
     * all of them: a handle exists before the bus does, and calls needing the bus
     * refuse by name. */
    ina219_handle_t i219 = NULL;
    ina219_config_t c219 = {.shunt_ohms = 0.1f, .max_current_a = 3.2f};
    ina219_report_t r219;
    expect(ina219_create(&c219, &i219, &r219) == ESP_OK, "ina219 handle created with no bus");
    expect(r219.calibration == 4194, "ina219 uses 0.04096, not the INA226's 0.00512");
    ina219_delete(i219);

    /* The same again over a different transport: ina239 is SPI, and its handle
     * has to be creatable before a bus exists just as the I2C ones are. */
    ina239_handle_t i239 = NULL;
    const ina239_config_t c239 = {.dev = NULL, .shunt_ohms = 0.01,
                                  .range = INA239_RANGE_41MV,
                                  .averaging = INA239_AVG_16};
    expect(ina239_create(&c239, &i239) == ESP_OK, "ina239 handle created with no bus");
    ina239_reading_t r239;
    expect(ina239_read(i239, &r239) == ESP_ERR_INVALID_STATE,
           "ina239 refuses without a device");
    expect(ina239_device_config(7, 0).mode == 1,
           "ina239 publishes its own SPI mode rather than leaving it to the caller");
    ina239_delete(i239);

    lm75bdp_handle_t lm = NULL;
    lm75bdp_config_t clm = {0};
    expect(lm75bdp_create(&clm, &lm, NULL) == ESP_OK, "lm75bdp handle created with no bus");
    lm75bdp_reading_t temp;
    expect(lm75bdp_read(lm, &temp) == ESP_ERR_INVALID_STATE, "lm75bdp refuses without a device");
    lm75bdp_delete(lm);

    rx8130ce_handle_t rtc = NULL;
    rx8130ce_config_t crtc = {0};
    expect(rx8130ce_create(&crtc, &rtc, NULL) == ESP_OK, "rx8130ce handle created with no bus");
    expect(!rx8130ce_power_was_lost(rtc), "rx8130ce reports no power loss before it is read");
    rx8130ce_delete(rtc);

    aw9523b_handle_t aw = NULL;
    aw9523b_config_t caw = {.port0_inputs = 0xF0, .port0_push_pull = true};
    expect(aw9523b_create(&caw, &aw, NULL) == ESP_OK, "aw9523b handle created with no bus");
    expect(aw9523b_set_pin(aw, AW9523B_PORT0, 0, true) == ESP_ERR_INVALID_STATE,
           "aw9523b refuses without a device");
    aw9523b_delete(aw);

    pi4ioe5v6408_handle_t io = NULL;
    pi4ioe5v6408_config_t cio = {.outputs = 0xF0, .pull_enable = 0x07, .pull_up = 0x07};
    expect(pi4ioe5v6408_create(&cio, &io, NULL) == ESP_OK, "pi4ioe5v6408 handle created with no bus");
    pi4ioe5v6408_delete(io);

    /* int_dispatch takes the pin as a parameter; the original compiled in GPIO 14. */
    expect(int_dispatch_register(NULL, NULL) == ESP_ERR_INVALID_ARG,
           "int_dispatch refuses a NULL handler");
    expect(int_dispatch_unserviced_count() == 0, "no unserviced interrupts yet");
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
    /*
     * Prerequisites, not conveniences. Every manager below registers a handler
     * on the default event loop, and http_server now opens its socket inside
     * start() -- which lwIP cannot serve until esp_netif_init() has brought the
     * TCP/IP thread up. Neither call joins a network or configures an interface,
     * which is the point: this test never has an address.
     */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    check_js2c();
    check_config();
    check_networking();
    check_mqtt();
    check_http_and_ota();
    check_drivers();

    ESP_ERROR_CHECK(diag_init(NULL));
    ESP_ERROR_CHECK(cli_register_group(&demo_group));

    printf(failures ? "\nCONSUMER TEST FAILED (%d)\n" : "\nCONSUMER TEST PASSED (%d)\n",
           failures);

    /* Brings up the shell on the serial console. Registering groups first means
     * the banner and tab completion see them. */
    ESP_ERROR_CHECK(cli_start(NULL));

    /*
     * cli_web is not started here. It no longer needs an address -- it would
     * come up on this server and simply drop output until a browser connected --
     * but starting it would leave a shell reachable by anyone who reaches the
     * device, which is a decision for an application rather than a default this
     * test should model. Referencing it is enough to prove it links, which is
     * what the build is checking.
     */
    (void)cli_web_start;
    (void)cli_web_stop;
    (void)cli_web_server;
}
