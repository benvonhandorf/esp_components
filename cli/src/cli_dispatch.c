#include "cli.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "diag.h"

#include "sdkconfig.h"

#define MAX_GROUPS CONFIG_CLI_MAX_GROUPS

/* Column at which a command's help text starts, so the listing lines up. */
#define HELP_SYNTAX_WIDTH 34

/* Kept sorted by name, so help and completion are alphabetical however the
 * groups were registered -- registration order is an accident of link order and
 * init sequence, and should not be visible to the user. */
static const cli_group_t *groups[MAX_GROUPS];
static size_t group_count;

const cli_group_t *cli_find_group(const char *name)
{
    for (size_t i = 0; i < group_count; i++) {
        if (strcasecmp(groups[i]->name, name) == 0) {
            return groups[i];
        }
    }
    return NULL;
}

static const cli_command_t *find_command(const cli_group_t *group, const char *name)
{
    for (size_t i = 0; i < group->command_count; i++) {
        if (strcasecmp(group->commands[i].name, name) == 0) {
            return &group->commands[i];
        }
    }
    return NULL;
}

esp_err_t cli_register_group(const cli_group_t *group)
{
    if (!group || !group->name || (group->command_count && !group->commands)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cli_find_group(group->name)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (group_count == MAX_GROUPS) {
        return ESP_ERR_NO_MEM;
    }

    /* Insertion sort: N is small and this runs once per group at startup. */
    size_t i = group_count;
    while (i > 0 && strcasecmp(groups[i - 1]->name, group->name) > 0) {
        groups[i] = groups[i - 1];
        i--;
    }
    groups[i] = group;
    group_count++;

    return ESP_OK;
}

void cli_print_help(const cli_group_t *group)
{
    if (!group) {
        diag_printf("\nCommand groups (type a group name to list its commands):\n\n");
        for (size_t i = 0; i < group_count; i++) {
            diag_printf("  %-*s %s\n", HELP_SYNTAX_WIDTH, groups[i]->name,
                        groups[i]->help ? groups[i]->help : "");
        }
        diag_printf("\nEvery command is '<group> <command>', e.g. 'gpio set 19 true'.\n");
        diag_printf("'help <group>' describes one group.\n\n");
        return;
    }

    if (group->help) {
        diag_printf("\n%s\n", group->help);
    }

    if (group->command_count == 0) {
        diag_printf("\n(no commands)\n\n");
        return;
    }

    diag_printf("\nCommands:\n");
    for (size_t i = 0; i < group->command_count; i++) {
        const cli_command_t *cmd = &group->commands[i];
        char syntax[80];
        snprintf(syntax, sizeof(syntax), "%s %s%s%s", group->name, cmd->name,
                 cmd->usage && cmd->usage[0] ? " " : "",
                 cmd->usage ? cmd->usage : "");
        diag_printf("  %-*s %s\n", HELP_SYNTAX_WIDTH, syntax,
                    cmd->help ? cmd->help : "");
    }
    diag_printf("\n");
}

static int run_help(int argc, char **argv)
{
    if (argc <= 1) {
        cli_print_help(NULL);
        return 0;
    }

    const cli_group_t *group = cli_find_group(argv[1]);
    if (!group) {
        diag_error("No such command group: %s", argv[1]);
        return -1;
    }

    cli_print_help(group);
    return 0;
}

int cli_execute(int argc, char **argv)
{
    if (argc == 0) {
        return 0;
    }

    if (strcasecmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        return run_help(argc, argv);
    }

    const cli_group_t *group = cli_find_group(argv[0]);
    if (!group) {
        diag_error("Unknown command group: %s", argv[0]);
        diag_printf("Type 'help' to list the groups.\n");
        return -1;
    }

    /* A bare group name lists what it holds. This is what replaces drilling in:
     * the user still discovers a group's contents by typing its name, but the
     * shell does not acquire a location as a result. */
    if (argc == 1) {
        cli_print_help(group);
        return 0;
    }

    const cli_command_t *cmd = find_command(group, argv[1]);
    if (!cmd) {
        diag_error("Unknown command: %s %s", group->name, argv[1]);
        diag_printf("Type '%s' to list its commands.\n", group->name);
        return -1;
    }

    /* argv[0] becomes the command's own name: the group token is consumed, and
     * a command sees exactly what it saw under the previous nested design. */
    return cmd->fn(argc - 1, &argv[1]);
}

size_t cli_complete(const char *line, const char **out, size_t out_max)
{
    if (!line || !out || out_max == 0) {
        return 0;
    }

    /* The token being completed is whatever follows the last space. */
    const char *token = strrchr(line, ' ');
    token = token ? token + 1 : line;

    /* Is anything but whitespace in front of it? If not, this is the group. */
    bool first_token = true;
    for (const char *p = line; p < token; p++) {
        if (*p != ' ') {
            first_token = false;
            break;
        }
    }

    size_t token_len = strlen(token);
    size_t count = 0;

    if (first_token) {
        for (size_t i = 0; i < group_count && count < out_max; i++) {
            if (strncasecmp(groups[i]->name, token, token_len) == 0) {
                out[count++] = groups[i]->name;
            }
        }
        if (count < out_max && strncasecmp("help", token, token_len) == 0) {
            out[count++] = "help";
        }
        return count;
    }

    /* Otherwise complete within the group named by the first token. */
    const char *start = line;
    while (*start == ' ') {
        start++;
    }
    const char *end = strchr(start, ' ');
    if (!end) {
        return 0;
    }

    char name[48];
    size_t name_len = (size_t)(end - start);
    if (name_len == 0 || name_len >= sizeof(name)) {
        return 0;
    }
    memcpy(name, start, name_len);
    name[name_len] = '\0';

    const cli_group_t *group = cli_find_group(name);
    if (!group) {
        return 0;
    }

    for (size_t i = 0; i < group->command_count && count < out_max; i++) {
        if (strncasecmp(group->commands[i].name, token, token_len) == 0) {
            out[count++] = group->commands[i].name;
        }
    }

    return count;
}
