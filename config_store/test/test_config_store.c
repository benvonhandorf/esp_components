/*
 * Host test for config_store. The component uses only stdio and paths, so this
 * runs against real files in a temporary directory -- the rotation logic is
 * exactly the code that runs on the device, not a model of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config_store.h"

static int failures;
static char dir[] = "/tmp/config_store_test_XXXXXX";
static char primary[256], override[256], backup[256], temp_path[256];

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

static void put(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool contents_are(const char *path, const char *expected)
{
    char buf[512] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, expected) == 0;
}

static void reset_files(void)
{
    unlink(primary); unlink(override); unlink(backup); unlink(temp_path);
}

static void init_primary_only(void)
{
    config_store_config_t cfg = {.path = primary, .max_size = 4096};
    config_store_init(&cfg);
}

static void test_read_prefers_the_override(void)
{
    reset_files();
    config_store_config_t cfg = {
        .path = primary, .override_path = override, .max_size = 4096,
    };
    expect(config_store_init(&cfg) == ESP_OK, "init");

    char buf[256];
    size_t len = 0;
    config_store_source_t source;

    expect(config_store_read(buf, sizeof(buf), &len, &source) == ESP_ERR_NOT_FOUND,
           "read with no file at all reports NOT_FOUND");
    expect(source == CONFIG_STORE_SOURCE_NONE, "and names no source");

    put(primary, "{\"from\":\"flash\"}");
    expect(config_store_read(buf, sizeof(buf), &len, &source) == ESP_OK, "reads the primary");
    expect(strcmp(buf, "{\"from\":\"flash\"}") == 0, "primary contents returned");
    expect(len == strlen("{\"from\":\"flash\"}"), "length excludes the terminator");
    expect(source == CONFIG_STORE_SOURCE_PRIMARY, "source is the primary");

    /* A config on removable media wins, so a device can be reconfigured without
     * reflashing it. */
    put(override, "{\"from\":\"sdcard\"}");
    expect(config_store_read(buf, sizeof(buf), &len, &source) == ESP_OK, "reads with override present");
    expect(strcmp(buf, "{\"from\":\"sdcard\"}") == 0, "override wins over the primary");
    expect(source == CONFIG_STORE_SOURCE_OVERRIDE, "source is the override");

    unlink(override);
    expect(config_store_read(buf, sizeof(buf), &len, &source) == ESP_OK, "falls back when override goes away");
    expect(source == CONFIG_STORE_SOURCE_PRIMARY, "back to the primary");
}

static void test_size_limits(void)
{
    reset_files();
    config_store_config_t cfg = {.path = primary, .max_size = 16};
    config_store_init(&cfg);

    put(primary, "0123456789012345678901234567890123456789");
    char buf[256];
    expect(config_store_read(buf, sizeof(buf), NULL, NULL) == CONFIG_STORE_ERR_TOO_LARGE,
           "a file over max_size is refused before being read");

    reset_files();
    init_primary_only();
    put(primary, "0123456789");
    char small[8];
    expect(config_store_read(small, sizeof(small), NULL, NULL) == CONFIG_STORE_ERR_TOO_LARGE,
           "a file that will not fit the caller's buffer is refused");

    char exact[11];
    expect(config_store_read(exact, sizeof(exact), NULL, NULL) == ESP_OK,
           "a file that exactly fits, with room for the terminator, is read");
}

static void test_write_rotates_and_keeps_the_previous(void)
{
    reset_files();
    init_primary_only();

    expect(config_store_write("{\"v\":1}", 7) == ESP_OK, "first write succeeds");
    expect(contents_are(primary, "{\"v\":1}"), "config written");
    expect(!config_store_has_backup(), "no backup after the first write");

    expect(config_store_write("{\"v\":2}", 7) == ESP_OK, "second write succeeds");
    expect(contents_are(primary, "{\"v\":2}"), "config replaced");
    expect(config_store_has_backup(), "the previous config became the backup");
    expect(contents_are(backup, "{\"v\":1}"), "backup holds the previous config");

    expect(!exists(temp_path), "no temporary file left behind");

    /* A third write rotates again rather than accumulating backups. */
    expect(config_store_write("{\"v\":3}", 7) == ESP_OK, "third write succeeds");
    expect(contents_are(backup, "{\"v\":2}"), "backup follows one version behind");
}

static void test_restore(void)
{
    reset_files();
    init_primary_only();

    expect(config_store_restore_backup() == ESP_ERR_NOT_FOUND,
           "restoring with no backup reports NOT_FOUND");

    config_store_write("{\"good\":1}", 10);
    config_store_write("{\"bad\":1}", 9);
    expect(contents_are(primary, "{\"bad\":1}"), "the bad config is live");

    expect(config_store_restore_backup() == ESP_OK, "restore succeeds");
    expect(contents_are(primary, "{\"good\":1}"), "the previous config is live again");
    expect(!config_store_has_backup(), "the backup was consumed by the restore");
}

static void test_guards(void)
{
    config_store_config_t bad = {.path = NULL};
    expect(config_store_init(&bad) == ESP_ERR_INVALID_ARG, "init refuses a NULL path");
    expect(config_store_init(NULL) == ESP_ERR_INVALID_ARG, "init refuses a NULL config");

    /* A path so long its .bak form would not fit is refused up front, rather
     * than at the first write. */
    char longpath[200];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    config_store_config_t toolong = {.path = longpath};
    expect(config_store_init(&toolong) == ESP_ERR_INVALID_SIZE,
           "a path whose .bak form would not fit is refused at init");

    init_primary_only();
    expect(config_store_read(NULL, 100, NULL, NULL) == ESP_ERR_INVALID_ARG,
           "read refuses a NULL buffer");
}

int main(void)
{
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(primary,   sizeof(primary),   "%s/config.json", dir);
    snprintf(override,  sizeof(override),  "%s/override.json", dir);
    snprintf(backup,    sizeof(backup),    "%s/config.json.bak", dir);
    snprintf(temp_path, sizeof(temp_path), "%s/config.json.new", dir);

    test_read_prefers_the_override();
    test_size_limits();
    test_write_rotates_and_keeps_the_previous();
    test_restore();
    test_guards();

    reset_files();
    rmdir(dir);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
