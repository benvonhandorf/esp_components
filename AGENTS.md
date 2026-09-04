# AGENTS.md

Orientation for anyone — human or agent — changing a component in this repository.

## What this is

A monorepo of reusable ESP-IDF components. Projects consume them by git tag, so the
components must not assume they are being built inside this repository, or alongside
any particular project. That single constraint drives every rule below.

Target ESP-IDF: **v6.0.2**. Components are **C11**, with every public header wrapped
in `extern "C"` so C++ projects can consume them.

## The component contract

These rules were learned the hard way, extracting drivers out of an application in
`esp_board_bringup`. They are not style preferences; each one has a failure behind it.

- **A component returns facts; the application formats prose.** No component calls an
  application's print or log helper. Every call that can produce an interesting
  outcome fills a small out-struct — including *which stage* failed — and the caller
  turns that into text. A log callback is the easy alternative and the wrong one: it
  moves the formatting decision into the component.
- **A call that collapses a sequence must report which step failed**, or the caller
  invents a wrong explanation and points the user at the wrong thing to fix.
- **A handle must be creatable before its bus exists.** Constructors accept a NULL
  device handle; every call that would talk to hardware returns
  `ESP_ERR_INVALID_STATE` until one arrives. A constructor that demands a live bus
  makes "what is this board wired like" commands impossible.
- **Guard order decides which error the user sees.** Check readiness before the bus,
  so the message names the command that is actually missing. Both are true; only one
  is useful.
- **The component takes handles, it does not find them.** Bus ownership, device-handle
  caches and their lifetimes stay with the application.
- **Ask what each file static is scoped to** before making it handle state. Something
  process-wide — the GPIO ISR service, for instance — stays static, or a second
  instance reports an error about a condition that is not a problem.
- **`REQUIRES` lists only what appears in the public header.** Everything else is
  `PRIV_REQUIRES`.
- **One `esp_err_t` base per component**, distinct from every other, so
  `esp_err_to_name()` cannot attribute a failure to the wrong part.
- **Each component sets its own `-Wall -Wextra -Werror`, PRIVATE on `COMPONENT_LIB`.**
  Project-wide flags would also hit ESP-IDF and managed components, which do not build
  clean.
- **Never write to a component's own source directory at build time.** A component
  pulled from `managed_components/` is hash-verified and re-extracted when
  dependencies change. Generated files go under `BUILD_DIR`.
- **Prefer explicit registration over weak symbols.** A `__attribute__((weak))`
  extension point that the application overrides with a strong symbol depends on link
  order, and breaks quietly once the component is an archive rather than a source file
  in the same tree. Pass a table or call a register function instead.
- **The hard-won comments travel with the code they annotate.** The register quirks
  and timing constraints are the reason a component is worth reusing.

## Layout of a component

```
<name>/
  CMakeLists.txt        idf_component_register + its own -Werror
  idf_component.yml     version, description, url/repository/issues, tags, idf floor
  README.md             what it is and how to use it
  CHANGELOG.md          Keep a Changelog, one entry per version bump
  include/<name>.h      the public interface; extern "C" wrapped
  src/<name>.c
  src/<name>_priv.h     anything the public header must not expose
  test/                 host tests where the logic can be tested off-target
  project_include.cmake only if the component exports CMake functions (see js2c)
```

## Proving a change

**An in-tree build proves nothing about reuse.** `project.cmake` gives `main` an
implicit dependency on every component in the build — but only while `main`'s
`REQUIRES` and `PRIV_REQUIRES` are both unset, which is why `main` must never set
either. So `main` can see everything, and a component that quietly depends on
something it never declared still links.

Build `tests/consumer/`, which sits outside every component and reaches them only
through `EXTRA_COMPONENT_DIRS`:

```sh
make -C js2c/test                                    # host tests, no IDF needed
cd tests/consumer && idf.py set-target esp32s3 && idf.py build
cd tests/consumer && idf.py set-target esp32c3 && idf.py build
```

## Versioning

One tag series per component: `<name>-v<major>.<minor>.<patch>`. Bump the version in
`idf_component.yml` and add a `CHANGELOG.md` entry in the same commit as the change.

## Publishing — required before the first release

**Inter-component dependencies are not yet declared in `idf_component.yml`.** Today the
build is driven only by `REQUIRES`/`PRIV_REQUIRES` in each `CMakeLists.txt`, which is what
actually links, and every component is found locally through `EXTRA_COMPONENT_DIRS`. That
is enough in this repository and not enough for anyone outside it: a project depending on
`cli` with `git:` + `path: cli` receives that directory only, and would not get `diag`.

So, at the point this repository is first pushed and tagged:

1. Create the per-component tags (`diag-v0.1.0`, `cli-v0.1.0`, …).
2. Add each sibling dependency to the dependent's `idf_component.yml`, e.g. `cli` gains
   `diag` with `git:`, `path: diag` and `version: diag-v0.1.0`. For a git dependency the
   component manager treats `version` as a **git ref**, not a semver range — which is why
   this cannot be done before the tags exist.

   Use `https://github.com/...` or `git@github.com:...`, never `git://`: that is the
   unauthenticated git daemon protocol on port 9418, which GitHub permanently disabled in
   2022, so it fails with a connection timeout that looks like a network fault.
3. Re-run `tests/consumer` against the published tags rather than
   `EXTRA_COMPONENT_DIRS`. Only that exercises the path a stranger takes, and it is the
   one part of the distribution model this repository cannot currently prove.

A component's **config schema is part of its public interface**, because the generated
struct's layout comes from it, and its `$id` names the generated type. Prefix that `$id`
with the component name: a schema `$id` of `wifi_config` generates `wifi_config_t`, which
is already ESP-IDF's own union for `esp_wifi_set_config()`. `wifi_manager_config` is both
unambiguous and more accurate — it is the manager's configuration, not the driver's. Adding an optional property that has a `default` is a
minor bump. Adding to `required`, renaming or removing a property, or changing
`maxLength`, `js2cType` or `type`, is a major bump — as is changing `$id`, which
renames the public type.

## Environment notes

- Sourcing `export.sh` puts the xtensa, riscv32 and esp32ulp toolchains ahead of
  `/usr/bin` on `PATH`, and their `ld` cannot link a host binary
  ("unrecognised emulation mode: elf_x86_64"). Host tests pass `-B/usr/bin` so they
  work regardless; anything new that compiles for the host must do the same.
- ESP-IDF's `linux` target needs `libbsd-dev` installed, and on v6.0.2 it also needs
  the build set narrowed (`set(COMPONENTS ...)` before `project()`), because
  `esp_driver_ana_cmpr` cannot resolve `esp_hal_ana_cmpr` there. Prefer plain-gcc host
  tests for logic that has no ESP-IDF dependency.
