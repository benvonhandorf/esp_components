# esp_components

Reusable ESP-IDF components, versioned independently and consumed from any project.

Each directory here is one component, with its own `idf_component.yml`, `README.md`
and `CHANGELOG.md`. A project depends on one by git tag rather than by copying it:

```yaml
# main/idf_component.yml
dependencies:
  js2c:
    git: git@github.com:benvonhandorf/esp_components.git
    path: js2c
    version: js2c-v0.1.0
```

Tags are per component (`js2c-v0.1.0`, `mqtt_manager-v0.2.1`), so upgrading one does
not drag in changes to the others.

## Components

| Component | What it does |
|---|---|
| [js2c](js2c/) | JSON Schema to C config parsers: vendored generator, jsmn runtime, and a CMake API components use to generate their own parser |
| [diag](diag/) | Output fan-out: one line reaches stdout and every registered sink, so serial, web and MQTT show the same text. Also captures `ESP_LOGx` |
| [cli](cli/) | Two-token command shell (`gpio set 19 true`) with registerable groups, one command executor, and no navigation state |
| [cli_web](cli_web/) | Browser transport for `cli`: an embedded terminal page and a WebSocket, sharing one session with the serial port |

## Testing

Two kinds, and the distinction matters:

- **`<component>/test/`** — host tests, plain `gcc`, no ESP-IDF and no hardware.
  `make -C js2c/test`, `make -C cli/test`. They compile the *real* sources, never a copy
  of them.
- **`tests/consumer/`** — an ESP-IDF project that lives outside every component and
  reaches them only through `EXTRA_COMPONENT_DIRS`. This is the acceptance test for
  reuse: an in-tree build cannot prove a component is self-contained, because `main`
  implicitly sees every component in the build.

```sh
make -C js2c/test
make -C cli/test
cd tests/consumer && idf.py set-target esp32s3 && idf.py build
```

See [tests/consumer/README.md](tests/consumer/README.md) for the full build matrix.

Read [AGENTS.md](AGENTS.md) before adding or changing a component.
