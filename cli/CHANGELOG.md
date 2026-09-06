# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Help and usage text may be supplied as ids instead of pointers, for a project
  that keeps its prose on a filesystem rather than in the image. A group carries
  an optional `command_text` array parallel to `commands[]`, plus its own
  `help_id`, and `cli_set_text_resolver()` says how an id becomes words.

  The ids live in a parallel array rather than in `cli_command_t` deliberately:
  those rows are written as positional initialisers, and appending to the struct
  would make every existing one a `-Wmissing-field-initializers` error under
  `-Wextra -Werror`. Existing tables are untouched, and with no resolver
  registered the ids are ignored, so a project without a string catalogue behaves
  exactly as before.

## [0.1.0] - 2026-09-04

### Added

- Two-token command shell: registerable groups, dispatch, help, tab completion, a single
  command executor, and the serial transport.
- Argument parsers for pin lists, integers, `0x`-prefixed numbers and doubles.
- Host tests covering dispatch, help, completion and parsing off target.

Extracted from `esp_board_bringup`'s `main/console/`.

### Changed from the original

- **Menus no longer nest and have no navigation.** `bp_menu_t.submenus`, the menu stack,
  the prompt path and `back`/`exit`/`quit` are gone; depth is expressed by hyphenating a
  group name (`i2c-nau7802`) so every line is two tokens. Roughly 200 of `menu.c`'s 340
  lines existed only to support standing inside a menu.
- **`bp_menu_execute_root()` is gone**, along with the reason it existed: with no current
  menu, a self-composed command line can no longer be captured by a command that shares
  its first token with wherever the user happens to be standing. Callers use
  `cli_execute()`.
- **Command shadowing is gone.** Dispatch used to try the current menu and then the root,
  so a command could shadow a same-named one depending on the user's location.
- **The root menu is registered, not linked.** `extern const bp_menu_t bp_root_menu` forced
  every consumer to define a central table and prevented a component from shipping its own
  commands; `cli_register_group()` replaces it. Registration is sorted, so help and
  completion are alphabetical regardless of call order.
- **Completion understands the group.** It used to match the first token against the
  current menu and the root; it now completes group names in the first position and that
  group's commands in the second, so `gpio r<tab>` offers only gpio's commands.
- **Output moved to `diag`.** `bp_printf`/`bp_error` are `diag_printf`/`diag_error`.
- **Parse helpers gained a `cli_` prefix.** A component's public header must not put a
  bare `parse_int_arg` into every consumer's namespace.
- Fixed sizes (`MAX_MENU_DEPTH`, line length, queue depth, task stacks, history) became
  Kconfig options.

### Migrating a nested command table

Command functions do not change: they still receive `argv[0] == their own name`. A submenu
becomes a top-level group whose name is `<parent>-<child>`, its `bp_menu_t` becomes a
`cli_group_t` with the `submenus`/`submenu_count` fields dropped, and each group is passed
to `cli_register_group()` instead of being listed in a parent's submenu array. Any command
line composed in C that named a submenu needs the hyphen.
