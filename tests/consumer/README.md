# Out-of-repo consumption test

An ESP-IDF project that lives outside every component and reaches them only through
`EXTRA_COMPONENT_DIRS`. That is the point: an in-tree build cannot prove a component is
self-contained, because `project.cmake` gives `main` an implicit dependency on every
component in the build, so a component that quietly relies on something it never declared
still links.

```sh
idf.py set-target esp32s3 && idf.py build
idf.py set-target esp32c3 && idf.py build

# cli_io.c picks its transport with #if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG, so a single
# build only ever compiles one half of it. The two commands above cover the UART branch;
# this covers the other one.
rm -rf build sdkconfig
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.ci.usb_jtag set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.ci.usb_jtag build
```

`main` deliberately declares no `REQUIRES` — that is what keeps its implicit dependency on
every component, and the doctrine in [AGENTS.md](../../AGENTS.md) requires it.

## What it covers

- **js2c** — `components/demo_net/` owns a schema and generates its parser from it,
  exercising `js2c_generate()` and `js2c_publish_schema()` from a component that is not in
  the same repository as `js2c`. `app_main()` asserts schema defaults, parsing, and
  range rejection with a readable reason.
- **cli / diag** — `main` registers a command group and starts the shell, so the
  dispatcher, the REPL, the serial transport and the output fan-out all link and run.
