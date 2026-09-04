#include <stdio.h>
#include <string.h>
#include "demo_net.h"
#include "js2c_error_capture.h"

static int failures;

static void expect(bool cond, const char *what) {
    printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

void app_main(void) {
    net_config_t cfg;

    /* Schema defaults are applied for absent optional fields. */
    const char *empty = "{}";
    memset(&cfg, 0xAA, sizeof(cfg));
    js2c_error_capture_reset();
    expect(!demo_net_parse(empty, strlen(empty), &cfg), "empty object parses");
    expect(cfg.channel == 6,        "channel defaulted from schema");
    expect(cfg.enabled == true,     "enabled defaulted from schema");
    expect(cfg.ssid[0] == '\0',     "ssid defaulted from schema");

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

    printf(failures ? "\nCONSUMER TEST FAILED (%d)\n" : "\nCONSUMER TEST PASSED (%d)\n", failures);
}
