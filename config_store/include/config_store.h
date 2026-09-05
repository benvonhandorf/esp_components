#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Durable storage for a device's configuration file.
 *
 * Deliberately knows nothing about filesystems. It takes paths and uses stdio,
 * so the application decides whether the config lives on LittleFS, FAT, an SD
 * card or a host filesystem under test, and mounts it. That keeps a filesystem
 * dependency out of every project that uses this, and it is the same rule the
 * drivers follow: the component takes what it is given, it does not go looking.
 *
 * What it does own is the part that is easy to get wrong -- replacing a config
 * file without being able to lose the old one, and preferring a removable copy
 * over the built-in one.
 */

#define CONFIG_STORE_ERR_BASE 0x3D000
/* The file is larger than the caller's buffer or than max_size. Distinct from
 * ESP_ERR_NO_MEM so a caller can tell "your buffer is too small" from "the
 * allocation failed". */
#define CONFIG_STORE_ERR_TOO_LARGE (CONFIG_STORE_ERR_BASE + 1)

typedef enum {
    CONFIG_STORE_SOURCE_NONE = 0,
    CONFIG_STORE_SOURCE_PRIMARY,   /* the canonical, writable path */
    CONFIG_STORE_SOURCE_OVERRIDE,  /* the read-first path, e.g. an SD card */
    CONFIG_STORE_SOURCE_BACKUP,    /* the previous config, after a restore */
} config_store_source_t;

typedef struct {
    /*
     * The canonical config file: read when no override is present, and the only
     * path ever written. Required.
     */
    const char *path;

    /*
     * Optional path consulted *before* `path` on read.
     *
     * This is how a config on removable media overrides the one built into the
     * firmware image: drop a file on an SD card to reconfigure a device without
     * reflashing it. Writes never go here -- a card can be pulled at any moment,
     * so the canonical copy stays the one the device maintains.
     */
    const char *override_path;

    /*
     * Refuse anything larger, before reading it. 0 means the caller's buffer is
     * the only limit. A config file is small; a bad path pointing at something
     * large should fail immediately rather than after filling memory.
     */
    size_t max_size;
} config_store_config_t;

/* Record the paths. Does not touch the filesystem, so it may be called before
 * anything is mounted. */
esp_err_t config_store_init(const config_store_config_t *cfg);

/*
 * Read the config into a caller-owned buffer.
 *
 * Tries `override_path` first, then `path`. The buffer is NUL-terminated, and
 * `*out_len` excludes the terminator, so the result can be handed straight to a
 * length-taking parser. `source` may be NULL; when given it says which file was
 * actually used, which is worth reporting -- "why is this device not using the
 * config I just wrote" is usually answered by "it found one on the SD card".
 *
 * ESP_ERR_NOT_FOUND if neither path exists.
 * CONFIG_STORE_ERR_TOO_LARGE if the file does not fit.
 */
esp_err_t config_store_read(char *buf, size_t buf_size, size_t *out_len,
                            config_store_source_t *source);

/*
 * Replace the canonical config, keeping the previous one.
 *
 * Writes a temporary file, then rotates: the existing config becomes the backup
 * and the temporary becomes the config. An interrupted write therefore loses the
 * new config rather than the working one, which is the right way round for a
 * device that has to boot again afterwards.
 *
 * The caller is expected to have parsed and accepted the bytes first: this
 * validates nothing, because what counts as valid is the project's schema, not
 * this component's business.
 */
esp_err_t config_store_write(const void *data, size_t len);

/* Promote the backup back to the canonical config, for when a newly written
 * config turns out not to work. ESP_ERR_NOT_FOUND if there is no backup. */
esp_err_t config_store_restore_backup(void);

/* Whether a backup exists. */
bool config_store_has_backup(void);

/*
 * Restart the device after a delay.
 *
 * A config written over HTTP usually has to take effect with a reboot, and
 * rebooting inside the request handler drops the response -- so the client is
 * told the write succeeded only if it never hears back. This schedules the
 * restart so the response can be sent first.
 */
esp_err_t config_store_schedule_restart(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */
