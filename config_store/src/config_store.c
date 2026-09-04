#include "config_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Suffixes appended to the canonical path. Kept short: LittleFS caps a name at
 * CONFIG_LITTLEFS_OBJ_NAME_LEN (64 by default), and the caller's path already
 * spends most of that. */
#define BACKUP_SUFFIX ".bak"
#define TEMP_SUFFIX   ".new"

#define MAX_PATH 128

static config_store_config_t s_cfg;
static bool s_ready;

static esp_err_t suffixed(const char *path, const char *suffix, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s%s", path, suffix);
    if (n < 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t config_store_init(const config_store_config_t *cfg)
{
    if (!cfg || !cfg->path || cfg->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* The suffixed forms have to fit too, and finding that out here beats
     * finding it out halfway through the first write. */
    char scratch[MAX_PATH];
    if (suffixed(cfg->path, BACKUP_SUFFIX, scratch, sizeof(scratch)) != ESP_OK ||
        suffixed(cfg->path, TEMP_SUFFIX, scratch, sizeof(scratch)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_cfg = *cfg;
    s_ready = true;
    return ESP_OK;
}

static esp_err_t read_file(const char *path, char *buf, size_t buf_size,
                           size_t *out_len, size_t max_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t size = (size_t)st.st_size;
    if (max_size && size > max_size) {
        return CONFIG_STORE_ERR_TOO_LARGE;
    }
    /* One byte kept back for the terminator, so the buffer can be handed to
     * string-taking code as well as to a length-taking parser. */
    if (size + 1 > buf_size) {
        return CONFIG_STORE_ERR_TOO_LARGE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t got = fread(buf, 1, size, f);
    fclose(f);

    if (got != size) {
        return ESP_FAIL;
    }

    buf[size] = '\0';
    if (out_len) {
        *out_len = size;
    }
    return ESP_OK;
}

esp_err_t config_store_read(char *buf, size_t buf_size, size_t *out_len,
                            config_store_source_t *source)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!buf || buf_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source) {
        *source = CONFIG_STORE_SOURCE_NONE;
    }

    /*
     * The override is consulted first but is not allowed to mask a real problem:
     * if it exists and cannot be read, that is reported rather than silently
     * falling through to the built-in config, which would make a corrupt card
     * look like a device ignoring it.
     */
    if (s_cfg.override_path && file_exists(s_cfg.override_path)) {
        esp_err_t err = read_file(s_cfg.override_path, buf, buf_size, out_len,
                                  s_cfg.max_size);
        if (err == ESP_OK && source) {
            *source = CONFIG_STORE_SOURCE_OVERRIDE;
        }
        return err;
    }

    esp_err_t err = read_file(s_cfg.path, buf, buf_size, out_len, s_cfg.max_size);
    if (err == ESP_OK && source) {
        *source = CONFIG_STORE_SOURCE_PRIMARY;
    }
    return err;
}

esp_err_t config_store_write(const void *data, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data && len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cfg.max_size && len > s_cfg.max_size) {
        return CONFIG_STORE_ERR_TOO_LARGE;
    }

    char temp[MAX_PATH];
    char backup[MAX_PATH];
    esp_err_t err = suffixed(s_cfg.path, TEMP_SUFFIX, temp, sizeof(temp));
    if (err != ESP_OK) {
        return err;
    }
    err = suffixed(s_cfg.path, BACKUP_SUFFIX, backup, sizeof(backup));
    if (err != ESP_OK) {
        return err;
    }

    FILE *f = fopen(temp, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    /* Flush through the VFS before the rotation below starts moving files
     * around, so a failure here is still a failure of the temporary file. */
    if (ok) {
        ok = (fflush(f) == 0);
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(temp);
        return ESP_FAIL;
    }

    /*
     * Rotate. rename() cannot portably replace an existing file on every VFS
     * ESP-IDF ships, so each destination is removed first. The order matters:
     * the old config becomes the backup before the new one takes its place, so
     * at no point are both gone.
     */
    if (file_exists(s_cfg.path)) {
        unlink(backup);
        if (rename(s_cfg.path, backup) != 0) {
            unlink(temp);
            return ESP_FAIL;
        }
    }

    if (rename(temp, s_cfg.path) != 0) {
        /* Put the previous config back rather than leaving the device with none. */
        rename(backup, s_cfg.path);
        unlink(temp);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool config_store_has_backup(void)
{
    if (!s_ready) {
        return false;
    }
    char backup[MAX_PATH];
    if (suffixed(s_cfg.path, BACKUP_SUFFIX, backup, sizeof(backup)) != ESP_OK) {
        return false;
    }
    return file_exists(backup);
}

esp_err_t config_store_restore_backup(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char backup[MAX_PATH];
    esp_err_t err = suffixed(s_cfg.path, BACKUP_SUFFIX, backup, sizeof(backup));
    if (err != ESP_OK) {
        return err;
    }
    if (!file_exists(backup)) {
        return ESP_ERR_NOT_FOUND;
    }

    unlink(s_cfg.path);
    if (rename(backup, s_cfg.path) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
