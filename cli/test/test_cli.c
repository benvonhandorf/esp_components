/*
 * Host test for the cli dispatcher and argument parsers.
 *
 * Dispatch is pure logic -- no FreeRTOS, no hardware -- so it is tested off
 * target against the real cli_dispatch.c and cli_args.c. Output is captured
 * rather than printed, so assertions can be made about the exact text a user
 * would see.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cli.h"
#include "diag.h"

static int failures;

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

static void expect_contains(const char *hay, const char *needle, const char *what)
{
    expect(strstr(hay, needle) != NULL, what);
    if (!strstr(hay, needle)) {
        printf("       looked for \"%s\" in:\n%s\n", needle, hay);
    }
}

/* --- a fake command set ---------------------------------------------------- */

static int last_argc;
static char last_argv0[32];
static char last_arg1[32];

static int cmd_record(int argc, char **argv)
{
    last_argc = argc;
    snprintf(last_argv0, sizeof(last_argv0), "%s", argv[0]);
    snprintf(last_arg1, sizeof(last_arg1), "%s", argc > 1 ? argv[1] : "");
    return 0;
}

static int cmd_fail(int argc, char **argv) { (void)argc; (void)argv; return -1; }

static const cli_command_t gpio_cmds[] = {
    {"set",  "<pin> <level>", "Drive a pin",  cmd_record},
    {"read", "<pin>",         "Read a pin",   cmd_record},
    {"blink", NULL,           "Blink a pin",  cmd_fail},
};
static const cli_group_t gpio_group = {
    .name = "gpio", .help = "Digital pin access", .commands = gpio_cmds,
    .command_count = sizeof(gpio_cmds) / sizeof(gpio_cmds[0]),
};

static const cli_command_t nau_cmds[] = {
    {"read", NULL, "Read the converter", cmd_record},
    {"init", NULL, "Power up",           cmd_record},
};
/* A hyphenated group: this is what a submenu becomes. */
static const cli_group_t nau_group = {
    .name = "i2c-nau7802", .help = "NAU7802 bridge ADC", .commands = nau_cmds,
    .command_count = sizeof(nau_cmds) / sizeof(nau_cmds[0]),
};

static const cli_command_t i2c_cmds[] = {
    {"scan", NULL, "Scan the bus", cmd_record},
};
static const cli_group_t i2c_group = {
    .name = "i2c", .help = "I2C master", .commands = i2c_cmds,
    .command_count = sizeof(i2c_cmds) / sizeof(i2c_cmds[0]),
};

/* --- helpers --------------------------------------------------------------- */

/* cli_execute() takes a tokenized line; split on spaces the simple way. */
static int run(const char *line)
{
    static char buf[256];
    static char *argv[16];
    snprintf(buf, sizeof(buf), "%s", line);

    int argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, " ", &save); t && argc < 16; t = strtok_r(NULL, " ", &save)) {
        argv[argc++] = t;
    }
    diag_capture_reset();
    return cli_execute(argc, argv);
}

/* --- tests ----------------------------------------------------------------- */

static void test_registration(void)
{
    expect(cli_register_group(&gpio_group) == ESP_OK, "register gpio");
    expect(cli_register_group(&nau_group)  == ESP_OK, "register i2c-nau7802");
    expect(cli_register_group(&i2c_group)  == ESP_OK, "register i2c");

    expect(cli_register_group(&gpio_group) == ESP_ERR_INVALID_STATE,
           "duplicate group name refused");
    expect(cli_register_group(NULL) == ESP_ERR_INVALID_ARG, "NULL group refused");

    expect(cli_find_group("GPIO") != NULL, "lookup is case-insensitive");
    expect(cli_find_group("nope") == NULL, "unknown group not found");
}

static void test_dispatch_preserves_the_argv_contract(void)
{
    /* The whole point: a command sees argv[0] == its own name, exactly as it
     * did when groups were nested menus. This is what lets ~80 existing command
     * functions move across untouched. */
    last_argc = 0;
    expect(run("gpio set 19 true") == 0, "two-token command dispatches");
    expect(last_argc == 3,                       "argc excludes the group token");
    expect(strcmp(last_argv0, "set") == 0,       "argv[0] is the command name");
    expect(strcmp(last_arg1, "19") == 0,         "argv[1] is the first real argument");

    /* A hyphenated group behaves identically -- it is not special-cased. */
    last_argc = 0;
    expect(run("i2c-nau7802 read") == 0, "hyphenated group dispatches");
    expect(last_argc == 1,                 "argc counts only the command");
    expect(strcmp(last_argv0, "read") == 0, "argv[0] is the command name");

    /* Group names are matched whole: "i2c" must not capture "i2c-nau7802". */
    last_argc = 0;
    expect(run("i2c scan") == 0, "i2c scan reaches the i2c group");
    expect(strcmp(last_argv0, "scan") == 0, "no prefix confusion between groups");

    expect(run("gpio blink 2") == -1, "a command's return value is propagated");
    expect(run("") == 0, "empty line is a no-op");
}

static void test_errors_name_the_fix(void)
{
    run("nosuch thing");
    expect_contains(diag_capture_text(), "ERR: ", "unknown group is an ERR: line");
    expect_contains(diag_capture_text(), "nosuch", "unknown group is named");
    expect_contains(diag_capture_text(), "help", "unknown group suggests help");

    run("gpio nosuch");
    expect_contains(diag_capture_text(), "ERR: ", "unknown command is an ERR: line");
    expect_contains(diag_capture_text(), "gpio nosuch", "unknown command is named in full");
}

static void test_help_and_bare_group(void)
{
    run("help");
    expect_contains(diag_capture_text(), "gpio",        "help lists gpio");
    expect_contains(diag_capture_text(), "i2c-nau7802", "help lists hyphenated groups");

    /* Groups are alphabetical however they were registered: gpio, i2c, i2c-nau7802. */
    const char *t = diag_capture_text();
    const char *g = strstr(t, "gpio");
    const char *i = strstr(t, "\n  i2c ");
    expect(g && i && g < i, "help lists groups alphabetically, not in registration order");

    run("help gpio");
    expect_contains(diag_capture_text(), "gpio set <pin> <level>",
                    "group help shows the full command syntax");
    expect_contains(diag_capture_text(), "Drive a pin", "group help shows the help text");

    /* A bare group name lists its commands. This is what replaces drilling in. */
    run("gpio");
    expect_contains(diag_capture_text(), "gpio read <pin>", "bare group lists its commands");

    expect(run("help nosuch") == -1, "help for an unknown group fails");
}

static void test_completion(void)
{
    const char *out[16];

    size_t n = cli_complete("i2c", out, 16);
    expect(n == 2, "'i2c' completes to both i2c groups");

    n = cli_complete("i2c-", out, 16);
    expect(n == 1 && strcmp(out[0], "i2c-nau7802") == 0,
           "'i2c-' narrows to the hyphenated group");

    /* Second token completes within the group named by the first. */
    n = cli_complete("gpio r", out, 16);
    expect(n == 1 && strcmp(out[0], "read") == 0, "'gpio r' completes gpio's commands");

    n = cli_complete("gpio ", out, 16);
    expect(n == 3, "'gpio ' lists all of gpio's commands");

    n = cli_complete("nosuch ", out, 16);
    expect(n == 0, "completion in an unknown group yields nothing");

    n = cli_complete("", out, 16);
    expect(n == 4, "empty line lists every group plus help");
}

static void test_arg_parsers(void)
{
    int v = 0;
    expect(cli_parse_int_arg("42", &v) == 0 && v == 42, "int parses");
    expect(cli_parse_int_arg("4x", &v) == -1, "int rejects trailing garbage");
    expect(cli_parse_int_arg("", &v) == -1,   "int rejects empty");

    expect(cli_parse_num_arg("0x40", &v) == 0 && v == 0x40, "0x prefix is hex");
    expect(cli_parse_num_arg("08", &v) == 0 && v == 8,
           "leading zero stays decimal, not octal");

    double d = 0;
    expect(cli_parse_double_arg("0.004", &d) == 0 && d > 0.003 && d < 0.005, "double parses");
    expect(cli_parse_double_arg("0.004ohm", &d) == -1, "double rejects trailing garbage");
    expect(cli_parse_double_arg("nan", &d) == -1, "double rejects NaN");

    int *pins = NULL, count = 0;
    expect(cli_parse_pin_list("1,4,8-10", &pins, &count) == 0 && count == 5,
           "pin list expands a range");
    if (pins) {
        expect(pins[0] == 1 && pins[2] == 8 && pins[4] == 10, "pin list values are right");
        free(pins);
    }
    expect(cli_parse_pin_list("5-1", &pins, &count) == -1, "reversed range refused");
    expect(cli_parse_pin_list("bad", &pins, &count) == -1, "non-numeric pin list refused");
}

/* --- help text supplied as ids rather than pointers ------------------------ */

static const cli_command_t res_cmds[] = {
    {"go", NULL, NULL, cmd_record},
};
static const cli_command_text_t res_text[] = {
    {101, 102},
};
static const cli_group_t res_group = {
    .name = "res",
    .commands = res_cmds,
    .command_count = sizeof(res_cmds) / sizeof(res_cmds[0]),
    .command_text = res_text,
    .help_id = 100,
};

static const char *fake_resolver(uint16_t id, char *buf, size_t buflen)
{
    switch (id) {
    case 100: snprintf(buf, buflen, "a group whose help is elsewhere"); return buf;
    case 101: snprintf(buf, buflen, "<arg>"); return buf;
    case 102: snprintf(buf, buflen, "a command whose help is elsewhere"); return buf;
    default:  return NULL;
    }
}

static void test_text_resolver(void)
{
    expect(cli_register_group(&res_group) == ESP_OK, "register a group with text ids");

    /* Without a resolver the ids mean nothing and the help is simply blank --
     * a project that has no string catalogue must still print a usable table. */
    run("help res");
    expect(strstr(diag_capture_text(), "res go") != NULL,
           "an unresolved group still lists its commands");
    expect(strstr(diag_capture_text(), "elsewhere") == NULL,
           "and prints no help text it cannot resolve");

    cli_set_text_resolver(fake_resolver);

    run("help res");
    expect_contains(diag_capture_text(), "a group whose help is elsewhere",
                    "group help comes from the resolver");
    expect_contains(diag_capture_text(), "res go <arg>",
                    "usage comes from the resolver and still forms the syntax line");
    expect_contains(diag_capture_text(), "a command whose help is elsewhere",
                    "command help comes from the resolver");

    /* The two lookups per row must not share a buffer. */
    const char *t = diag_capture_text();
    const char *syntax = strstr(t, "res go <arg>");
    expect(syntax && strstr(syntax, "a command whose help is elsewhere") != NULL,
           "usage and help resolve into separate buffers on one line");

    /* A literal still wins over an id, so a mixed table behaves. */
    run("help gpio");
    expect_contains(diag_capture_text(), "Drive a pin",
                    "a literal is still used when one is given");

    cli_set_text_resolver(NULL);
}

int main(void)
{
    test_registration();
    test_dispatch_preserves_the_argv_contract();
    test_errors_name_the_fix();
    test_help_and_bare_group();
    test_completion();
    test_text_resolver();
    test_arg_parsers();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
