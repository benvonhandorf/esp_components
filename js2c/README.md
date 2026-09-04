# js2c

JSON Schema to C: generate a config struct and a parser for it from a JSON Schema, so
a component's configuration has one definition, is documented and range-checked, and
can be validated in an editor before it ever reaches a device.

Wraps [json_schema_to_c](https://github.com/badicsalex/json_schema_to_c) (MIT), which
is **vendored** under `tool/` at revision `5700756`, together with the jsmn runtime the
generated parsers need.

## Using it from a component

```cmake
# The guard is mandatory. ESP-IDF re-runs every component CMakeLists.txt in `cmake -P`
# script mode to collect requirements, and js2c_generate() does not exist in that pass.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate("${CMAKE_CURRENT_SOURCE_DIR}/wifi_config_schema.json"
                  BASENAME wifi_config OUT_C GEN_C OUT_H GEN_H)
    get_filename_component(GEN_DIR "${GEN_C}" DIRECTORY)
endif()

idf_component_register(SRCS "wifi_manager.c" "${GEN_C}"
                       INCLUDE_DIRS "include" "${GEN_DIR}"
                       REQUIRES js2c)

# LOG_ERROR must be in scope before js2c_builtins.h, but only inside the generated
# translation unit -- the component's own sources keep their normal error handling.
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
```

A schema with `"$id": "wifi_config"` yields `wifi_config_t` and
`json_parse_wifi_config[_with_len]()`. `_with_len` parses a slice in place, with no
copy — prefer it.

`js2c_generate()` runs at **configure** time and writes only under `BUILD_DIR`, never
into the component's source directory: a component pulled from `managed_components/`
is hash-verified and re-extracted. Configure time rather than build time because the
generated header is public, and a build-time rule gives a dependent's *compile* step
no ordering edge to the generator — only its link step — so a dependent that includes
the header would race it on a clean parallel build.

## Composing a project config

A project writes **one** schema. Each top-level property that `$ref`s another component's
schema is a *section*:

```json
{
  "$id": "app_config",
  "required": ["config_version", "wifi", "mqtt"],
  "properties": {
    "config_version": { "type": "integer", "minimum": 1, "maximum": 1 },
    "wifi": { "$ref": "wifi_manager/wifi_config_schema.json" },
    "mqtt": { "$ref": "mqtt_manager/mqtt_config_schema.json" }
  }
}
```

```cmake
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate_sections("${CMAKE_CURRENT_LIST_DIR}/../config/app_config_schema.json"
                           BASENAME app_config OUT_C GEN_C)
    get_filename_component(GEN_DIR "${GEN_C}" DIRECTORY)
endif()
```

Do not name an output variable after one of these functions' own keywords
(`BASENAME`, `GEN_DIR`, `SCHEMA`, ...). `cmake_parse_arguments` treats a value that
matches a keyword as a keyword, so `OUT_C GEN_DIR` parses as two empty options and the
variable is silently never set.

The first path segment of a `$ref` is the **component that owns the fragment**, resolved to
that component's directory — so the reference says nothing about where the component
actually lives, whether that is `components/`, a namespaced directory under
`managed_components/`, or anywhere `EXTRA_COMPONENT_DIRS` points.

This cannot be generated from the authored schema directly: `json_schema_to_c` would emit a
second definition of every section's struct, colliding with the one the owning component
already generates. So `js2c_generate_sections()` rewrites each section to
`{"js2cType": "raw"}` — the walker records that section's byte offset and length, and the
project hands the slice to the parser belonging to the component that owns it, in place:

```c
#define SECTION_PARSER_wifi json_parse_wifi_config_with_len
#define SECTION_PARSER_mqtt json_parse_mqtt_config_with_len

#define PARSE_SECTION(name)                                              \
    if (SECTION_PARSER_##name(json + cfg.top.name.index,                 \
                              cfg.top.name.length, &cfg.name)) { ... }

APP_CONFIG_SECTIONS(PARSE_SECTION)   /* generated X-macro over the sections */
```

Dispatch through the X-macro, not by hand: a section added to the schema expands to an
`X()` naming a parser and a struct member that do not exist yet, so a forgotten section is
a build failure rather than a silently zeroed struct.

Every section must be listed in `required` — a raw field cannot carry a default, so an
absent one would leave the slice zeroed instead of reporting a problem. Write `{}` to mean
"all defaults".

### The token budget

The walker costs one token per section, but the tokenizer still has to hold every token of
the content it is skipping. So `js2c_generate_sections()` computes the budget from the
authored schema with every section inlined — using the generator's own accounting rather
than a reimplementation — and embeds it in the derived schema's `js2cSettings`. Nothing to
pick by hand, and nothing that drifts when a fragment grows.

## Writing a schema

- **Give it an `$id`.** It names the generated type, and it is public API.
- **Every property must be `required` or have a `default`.** The generator refuses
  otherwise, which is the point: defaults live in the schema, in one place, and an
  absent optional field still yields a populated struct.
- **Set `additionalProperties: false`**, or pass `--allow-additional-properties N` via
  `EXTRA_ARGS`. That switch is global to a schema, so use it only where the schema has
  a single object.
- **`maxLength` is ABI**: `maxLength: 32` becomes `char[33]`.
- **Integers default to `uint64_t`.** Use `"js2cType": "uint8_t"` and friends, or a
  `channel` constrained to 1..13 costs eight bytes.
- **`"js2cType": "raw"`** stores `{index, length}` — a slice of the input rather than a
  parsed value. This is how a top-level config walker hands each section to the
  component that owns it without copying, and without the walker needing to know that
  component's type.

### Referring to another schema file

Cross-file `$ref` works, authorized by `--authorized-paths` (`js2c_generate()` allows
the staged schema root automatically) and resolved relative to the referring file.
It must point at a **named node**, not another file's root:

```json
{ "$ref": "shared_defs.json#/$defs/port" }   /* works                        */
{ "$ref": "shared_defs.json" }               /* rejected: needs a fragment   */
{ "$ref": "shared_defs.json#/" }             /* crashes the generator        */
```

So a fragment meant to be referenced should expose its object under `$defs`.

## Error reporting

The generated parsers report *why* a document was rejected through a `LOG_ERROR`
macro that is empty by default. `js2c_error_capture` fills it in:

```c
js2c_error_capture_reset();
if (json_parse_wifi_config_with_len(body, len, &cfg)) {
    const char *why = js2c_error_capture_get();
    /* e.g. "Integer 99 in 'channel' out of range. It must be <= 13. (at byte 11)" */
}
```

The buffer is a single static, so parses whose errors you intend to read must not be
interleaved across tasks.

## Tests

```sh
make -C test
```

Plain gcc, no ESP-IDF, no hardware. It also checks that `include/jsmn.h` and
`include/js2c_builtins.h` are byte-identical to the vendored generator's own copies —
the two are a matched pair with no version check, and a predecessor project let them
drift to a different upstream jsmn without noticing.
