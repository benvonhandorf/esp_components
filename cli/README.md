# cli

A command shell for bring-up and field diagnostics, reachable over the serial port and —
with [`cli_web`](../cli_web/) — a browser, at the same time, with output from either
appearing on both.

```c
static int cmd_gpio_set(int argc, char **argv);   /* argv[0] == "set" */

static const cli_command_t gpio_cmds[] = {
    {"set",  "<pin> <level>", "Drive a pin as an output", cmd_gpio_set},
    {"read", "<pin>",         "Read a pin",               cmd_gpio_read},
};
static const cli_group_t gpio_group = {
    "gpio", "Digital pin access", gpio_cmds, sizeof(gpio_cmds)/sizeof(gpio_cmds[0]),
};

cli_register_group(&gpio_group);
cli_start(NULL);
```

## Groups are names, not places

Every command line is exactly two tokens — a group and a command:

```
gpio set 19 true          i2c-nau7802 read        audio-nau8822 volume 50
```

You do not *enter* a group. There is no current group, no prompt path, no
`back`/`exit`/`quit`. Deeper structure is expressed by hyphenating the group name, which
is why the line is always two tokens however the hardware is organised. Typing a bare
group name lists its commands, which is how you discover what a group holds without the
shell acquiring a location as a result.

This is a deliberate reversal of a nested design. Navigation cost ~200 lines of state and
carried two failure modes: a command in the current menu could **shadow** a same-named
command at the root depending on where the user was standing, and a command line composed
by the firmware itself could be **captured by the very command running it**, recursing
without bound. Neither is fixed here — neither can occur.

A command receives `argv[0] == its own name`: the group token is consumed by the
dispatcher, so `gpio set 19 true` arrives as `argc=3, argv={"set","19","true"}`.

## One executor

Every interface funnels through `cli_submit()` into a single executor task, so **commands
never run concurrently and command modules need no locking of their own.** Code already on
the executor — a board preset replaying a line it composed — calls `cli_execute()` directly
instead, because submitting would deadlock waiting for itself.

## Conventions for command code

- **Never call `printf()`** — use `diag_printf()`, or the output never leaves the serial
  port. `ESP_LOGx` is already routed through the same fan-out.
- **Report failures with `diag_error()`**, which prefixes `ERR:` so a host-side script can
  detect failure without parsing prose, and **return -1**; return 0 on success.
- **Parse with the helpers here, never `atoi()`**: `cli_parse_pin_list()` (accepts `4`,
  `0-5`, `1,4,8-10`; caller frees), `cli_parse_int_arg()`, `cli_parse_num_arg()` (`0x`
  means hex; a bare `08` stays decimal), `cli_parse_double_arg()`. They reject trailing
  garbage, so `gpio read foo` is an error rather than a read of pin 0.
- Keep output line-oriented and script-friendly: one record per line.

## What it borrows from ESP-IDF, and what it does not

It `REQUIRES` IDF's `console` component for **linenoise** (line editing, history,
dumb-terminal probe) and **`esp_console_split_argv()`** (quoting and escapes, so
`uart send "hello world"` is one argument).

It does not use `esp_console_cmd_register()`/`esp_console_run()` — a flat registry with no
grouping — nor `argtable3`, which cannot model argument syntax like `1,4,8-10`. Nor
`esp_console_new_repl_*()`, tempting though it is: its loop owns the reader task and calls
`esp_console_run()` directly, so a line arriving over a WebSocket would have nowhere to go,
and the single-executor property would be lost.

One consequence of declining the packaged REPL: `linenoiseSetReadCharacteristics()` is
strongly overridden only in `esp_console_repl_internal.c`, which the linker pulls in only
if you reference that REPL. This component gets linenoise's weak, blocking read — which is
what a dedicated reader task wants, but it means there is no way to unblock a reader
sitting in `linenoise()`, so there is no `cli_stop()`.

## Tests

```sh
make -C test
```

Plain gcc, no ESP-IDF and no hardware: dispatch and argument parsing are pure logic, so
the test compiles the real `cli_dispatch.c` and `cli_args.c` against small stubs and
asserts on captured output. Serial behaviour is verified by building
[`tests/consumer`](../tests/consumer/) for both console transports.
