#ifndef CLI_H
#define CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A command shell for bring-up and field diagnostics, reachable over the serial
 * port and -- via the cli_web component -- a browser, at the same time.
 *
 * Commands are addressed as two tokens, a group and a command:
 *
 *     gpio set 19 true
 *     i2c-nau7802 read
 *     audio-nau8822 volume 50
 *
 * Groups are a naming, help and completion device only. You do not "enter" one:
 * there is no current group, no prompt path and no back/exit. Deeper structure
 * is expressed by hyphenating the group name, which is why every command line is
 * exactly two tokens regardless of how the hardware is organised.
 *
 * That is a deliberate reversal of an earlier design in which menus nested and
 * you could stand inside one. Navigation cost roughly 200 lines of state and
 * brought two failure modes with it: a command in the current menu could shadow
 * a same-named command at the root depending on where the user happened to be
 * standing, and a command line composed by the firmware itself could be captured
 * by the very command that was running it, recursing without bound.
 *
 * A command receives argv[0] == its own name, so `gpio set 19 true` reaches
 * cmd_gpio_set() as argc=3, argv={"set","19","true"} -- the group token is
 * consumed by the dispatcher.
 */

typedef int (*cli_command_fn)(int argc, char **argv);

typedef struct {
    const char *name;  /* "set" */
    const char *usage; /* argument syntax only, without group or command name */
    const char *help;  /* one line */
    cli_command_fn fn;
} cli_command_t;

/*
 * Where a command's help text lives when it is not a literal.
 *
 * A project that keeps its prose on a filesystem rather than in the image
 * leaves .usage and .help NULL and supplies one of these per command, so the
 * table carries two bytes instead of two strings. Resolution goes through
 * cli_set_text_resolver(); without a resolver these are ignored and the
 * pointers are all that matter.
 *
 * It is a parallel array rather than two more fields in cli_command_t
 * deliberately: those rows are written as positional initialisers all over the
 * place, and appending to the struct would make every one of them a
 * -Wmissing-field-initializers error under -Wextra -Werror. This way an
 * existing table is untouched, and so is every project that has one.
 */
typedef struct {
    uint16_t usage_id; /* 0 for none */
    uint16_t help_id;  /* 0 for none */
} cli_command_text_t;

typedef struct {
    const char *name;  /* "gpio", "i2c-nau7802" */
    const char *help;  /* one line */
    const cli_command_t *commands;
    size_t command_count;
    /* NULL, or command_count entries parallel to commands[]. */
    const cli_command_text_t *command_text;
    uint16_t help_id;  /* used when help is NULL; 0 for none */
} cli_group_t;

/*
 * Resolve a text id into caller-supplied storage, returning buf, or NULL if the
 * id names nothing. Registering one is what makes the ids above mean anything.
 *
 * Deliberately a callback rather than a dependency: the same reasoning that
 * keeps diag's sinks with their transports keeps the string catalogue out of
 * this component, so cli still builds in a project that has no such thing.
 */
typedef const char *(*cli_text_resolver_fn)(uint16_t id, char *buf, size_t buflen);

void cli_set_text_resolver(cli_text_resolver_fn fn);

typedef struct {
    /* Printed once a terminal attaches. NULL for a default naming the chip. */
    const char *banner;
    /* NULL for a default of "<target>> ". Must stay valid for the process. */
    const char *prompt;
} cli_config_t;

/*
 * Add a group to the shell. The group and its command table must stay valid for
 * the life of the process -- pass statics, not stack.
 *
 * Groups may be registered by components as well as by the application, which is
 * the point: nothing has to maintain a central table of every command in the
 * firmware. Registration is sorted by name, so help and completion come out in a
 * stable alphabetical order regardless of call order.
 *
 * ESP_ERR_NO_MEM if the table is full (CONFIG_CLI_MAX_GROUPS),
 * ESP_ERR_INVALID_STATE if a group of the same name is already registered.
 */
esp_err_t cli_register_group(const cli_group_t *group);

/* Start the executor and the serial reader. Register groups first, so the
 * banner and completion see them. `cfg` may be NULL for defaults. */
esp_err_t cli_start(const cli_config_t *cfg);

/*
 * Run one already-tokenized command line, on the calling task.
 *
 * Returns the command's own return value, or -1 if the line could not be
 * resolved. `help` and a bare group name return 0.
 *
 * Callers running lines they composed themselves -- board presets and the like
 * -- use this directly rather than cli_submit(), because they are already on the
 * executor task and submitting would deadlock waiting for themselves.
 */
int cli_execute(int argc, char **argv);

/*
 * Queue a line for the executor task, optionally blocking until it completes.
 *
 * Every interface funnels through here, so commands never run concurrently and
 * command modules need no locking of their own. Returns 0 on success, -1 if the
 * queue is full or the shell is not started.
 */
int cli_submit(const char *line, bool wait);

/* The prompt string. Constant for the life of the process. */
const char *cli_prompt(void);

/*
 * Tab-completion candidates for the final (possibly empty) token of `line`.
 *
 * Completes group names when the final token is the first one, and that group's
 * command names otherwise, so "i2c-<tab>" lists every I2C part and
 * "gpio s<tab>" lists only gpio's commands. Returned pointers are the registry's
 * own strings and remain valid.
 */
size_t cli_complete(const char *line, const char **out, size_t out_max);

/* Print the group list, or one group's commands. NULL prints the group list. */
void cli_print_help(const cli_group_t *group);

/* Look up a registered group by name, case-insensitively. NULL if absent. */
const cli_group_t *cli_find_group(const char *name);

/*
 * Argument parsing.
 *
 * Command code uses these rather than atoi(): they must consume their entire
 * token, so `gpio read foo` is an error rather than a read of pin 0.
 */

/*
 * Parse a pin specification into a caller-owned array of pin numbers.
 *
 * Accepts a single pin ("4"), an inclusive range ("0-5"), or a comma separated
 * list of either ("1,4,8-10"). On success *pins must be free()d by the caller.
 * Returns 0 on success, -1 on a malformed specification (in which case *pins is
 * NULL and *count is 0).
 */
int cli_parse_pin_list(const char *pin_str, int **pins, int *count);

/* Parse a whole token as a decimal integer. 0 on success, -1 otherwise. */
int cli_parse_int_arg(const char *token, int *out);

/*
 * As cli_parse_int_arg(), but a "0x"/"0X" prefix selects hexadecimal.
 * Everything else is decimal -- deliberately *not* strtol()'s base 0, under
 * which a bare I2C address like "08" would be an invalid octal constant.
 */
int cli_parse_num_arg(const char *token, int *out);

/*
 * Parse a whole token as a finite double, e.g. a shunt resistance of "0.004".
 * Rejects trailing garbage, infinities and NaN. 0 on success, -1 otherwise.
 */
int cli_parse_double_arg(const char *token, double *out);

#ifdef __cplusplus
}
#endif

#endif /* CLI_H */
