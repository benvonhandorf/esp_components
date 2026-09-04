# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-03

### Added

- Vendored `json_schema_to_c` at upstream revision `570075656a080a6756add4dc209d8f8d5ed4c0da`
  under `tool/`, MIT licensed (see `tool/LICENSE.json_schema_to_c`).
- `project_include.cmake` exporting `js2c_generate()` and `js2c_generate_sections()`.
- `js2c_sections.py`, which derives a project's top-level config walker from its one
  authored schema: each section that `$ref`s a component's fragment becomes a
  `js2cType: "raw"` slice, so the section is parsed in place by the component that owns
  it rather than being defined a second time here. It also emits an X-macro over the
  sections, so one added to the schema but not dispatched in C fails to compile, and
  computes the jsmn token budget from the inlined schema using the generator's own
  accounting.

  A section names its fragment by the **component that owns it**
  (`wifi_manager/wifi_config_schema.json`), resolved to that component's directory. An
  earlier attempt had each component copy its fragment to a staging directory, which does
  not work: ESP-IDF gives `main` an implicit dependency on every component but still
  configures it before most of them, so the fragments are not there yet when the project's
  schema is derived. Component directories are known before any component's
  `CMakeLists.txt` runs, so resolving through them has no ordering dependency.
- `js2c_error_capture` runtime, which turns the diagnostics the generated parsers
  emit through `LOG_ERROR` into a readable reason string.

### Notes on why this is vendored rather than fetched

The predecessor of this component fetched the generator with
`FetchContent_Declare(... GIT_HASH edb272e...s)`. `GIT_HASH` is not a
`FetchContent_Declare` keyword, so the pin never took effect and the build silently
tracked the upstream default branch. The effect was not theoretical: the nominally
pinned revision `edb272e` has no `_with_len` parser variant at all, while the code
that consumed the generated output called `json_parse_ntp_config_schema_with_len()`.
Had the pin worked, that project would not have compiled. Vendoring makes the
generator and its runtime headers one artifact, versioned by this component's tag
and hash-verified in `dependencies.lock`, and makes the build work offline.

`include/jsmn.h` and `include/js2c_builtins.h` are copied from the generator's own
`tool/js2c/codegen/` rather than from the predecessor component, whose `jsmn.h` had
drifted to a different upstream jsmn (enum values `1 << 0 .. 1 << 3` instead of
`1 .. 4`, and named struct tags). Nothing had broken yet only because both halves of
the pair happened to come from the same file.
