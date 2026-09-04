#include <stdio.h>
#include <string.h>

#include "cli.h"
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

    ESP_ERROR_CHECK(diag_init(NULL));
    ESP_ERROR_CHECK(cli_register_group(&demo_group));

    printf(failures ? "\nCONSUMER TEST FAILED (%d)\n" : "\nCONSUMER TEST PASSED (%d)\n",
           failures);

    /* Brings up the shell on the serial console. Registering groups first means
     * the banner and tab completion see them. */
    ESP_ERROR_CHECK(cli_start(NULL));
}
