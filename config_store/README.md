# config_store

Durable storage for a device's configuration file: read it, replace it without being able
to lose the working copy, and let removable media override it.

```c
const config_store_config_t cfg = {
    .path          = "/res/config.json",     /* canonical, writable */
    .override_path = "/sdcard/config.json",  /* optional, read first */
    .max_size      = 4096,
};
config_store_init(&cfg);

char buf[4096];
size_t len;
config_store_source_t source;
if (config_store_read(buf, sizeof(buf), &len, &source) == ESP_OK) {
    /* buf is NUL-terminated; len excludes the terminator, so it suits a
     * length-taking parser as well as string code. */
}
```

## It does not mount anything

The application chooses and mounts the filesystem; this takes paths and uses stdio. That
keeps a LittleFS dependency out of every project that happens to store a config file —
yours might use FAT, or an SD card, or a host filesystem under test — and it is the same
rule the drivers follow: the component takes what it is given rather than going looking.

It also means the file handling is tested on the host **against real files**, so the
rotation logic that runs on the device is the code the tests exercise.

```c
esp_vfs_littlefs_conf_t conf = {
    .base_path = "/res", .partition_label = "res", .format_if_mount_failed = true,
};
esp_vfs_littlefs_register(&conf);        /* the application's decision */
config_store_init(&cfg);
```

## Writing cannot lose the working config

`config_store_write()` writes a temporary file, then rotates: the existing config becomes
`<path>.bak`, and the temporary becomes the config. An interrupted write therefore loses
the **new** config rather than the working one, which is the right way round for a device
that has to boot again afterwards. If the final rename fails, the previous config is put
back rather than leaving the device with none.

It validates nothing. What counts as valid is the project's schema, so parse and accept
the bytes first, then store them.

One backup, not a timestamped series: a partition with no quota should not accumulate
files, and a timestamp needs a clock that may not be set yet.

## The override path

`override_path` is consulted **before** `path` on read, and is never written. That is how
a config on removable media reconfigures a device without reflashing it, while the
canonical copy stays the one the device maintains — a card can be pulled at any moment.

If the override exists but cannot be read, that is reported rather than quietly falling
through to the built-in config: a corrupt card should not look like a device ignoring it.
`config_store_read()` reports which file it actually used, which is usually the answer to
"why is this device not using the config I just wrote".

## Rebooting after a write

```c
config_store_write(body, len);
config_store_schedule_restart(500);   /* let the HTTP response go out first */
```

Rebooting inside a request handler drops the response, so the client is told the write
succeeded only if it never hears back.

## Tests

```sh
make -C test
```

Plain gcc, no ESP-IDF and no hardware: the test creates a temporary directory and
exercises reads, size limits, rotation, restore and the guards against real files.
