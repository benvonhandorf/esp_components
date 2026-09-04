#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_io.h"
#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "linenoise/linenoise.h"

#include "sdkconfig.h"

#define MAX_LINE_LEN      CONFIG_CLI_MAX_LINE_LEN
#define MAX_ARGS          CONFIG_CLI_MAX_ARGS
#define COMMAND_QUEUE_LEN CONFIG_CLI_COMMAND_QUEUE_LEN
#define EXECUTOR_STACK    CONFIG_CLI_EXECUTOR_STACK
#define READER_STACK      CONFIG_CLI_READER_STACK
#define PROMPT_BUF_SIZE   32

typedef struct {
    char *line;             /* heap-allocated; the executor frees it */
    SemaphoreHandle_t done; /* optional; given once the command completes */
} command_item_t;

static QueueHandle_t command_queue;
static const char *banner_text;
static const char *prompt_text;
static char prompt_buf[PROMPT_BUF_SIZE];

const char *cli_prompt(void)
{
    return prompt_text ? prompt_text : "> ";
}

/*
 * One task runs every command, whatever interface it arrived from. That is the
 * property the rest of the firmware leans on: commands never run concurrently,
 * so command modules need no locking of their own. It is also why the shell does
 * not use esp_console_start_repl(), whose loop both owns the reader task and
 * dispatches directly, leaving a line that arrived over a WebSocket nowhere to go.
 */
static void executor_task(void *arg)
{
    (void)arg;

    command_item_t item;
    char *argv[MAX_ARGS];

    while (xQueueReceive(command_queue, &item, portMAX_DELAY) == pdTRUE) {
        /* esp_console_split_argv() is the tokenizer from IDF's console
         * component: it understands quoting and backslash escapes, so
         * `uart send "hello world"` arrives as a single argument. It
         * rewrites the buffer in place. */
        size_t argc = esp_console_split_argv(item.line, argv, MAX_ARGS);

        if (argc > 0) {
            cli_execute((int)argc, argv);
        }

        free(item.line);

        if (item.done) {
            xSemaphoreGive(item.done);
        }
    }
}

int cli_submit(const char *line, bool wait)
{
    if (!command_queue || !line) {
        return -1;
    }

    command_item_t item = {
        .line = strdup(line),
        .done = NULL,
    };
    if (!item.line) {
        return -1;
    }

    SemaphoreHandle_t done = NULL;
    if (wait) {
        done = xSemaphoreCreateBinary();
        if (!done) {
            free(item.line);
            return -1;
        }
        item.done = done;
    }

    if (xQueueSend(command_queue, &item, pdMS_TO_TICKS(5000)) != pdTRUE) {
        diag_error("Shell busy, command dropped: %s", line);
        free(item.line);
        if (done) {
            vSemaphoreDelete(done);
        }
        return -1;
    }

    if (done) {
        xSemaphoreTake(done, portMAX_DELAY);
        vSemaphoreDelete(done);
    }

    return 0;
}

/* Tab completion for the token currently being typed. */
static void completion_cb(const char *buf, linenoiseCompletions *lc)
{
    const char *token = strrchr(buf, ' ');
    token = token ? token + 1 : buf;
    size_t prefix_len = (size_t)(token - buf);

    const char *candidates[32];
    size_t count = cli_complete(buf, candidates, 32);

    for (size_t i = 0; i < count; i++) {
        char completed[MAX_LINE_LEN];
        snprintf(completed, sizeof(completed), "%.*s%s",
                 (int)prefix_len, buf, candidates[i]);
        linenoiseAddCompletion(lc, completed);
    }
}

static void print_banner(void)
{
    diag_printf("\n");
    if (banner_text) {
        diag_printf("%s\n", banner_text);
    } else {
        diag_printf("%s shell (console: %s)\n", CONFIG_IDF_TARGET, cli_io_name());
    }
    diag_printf("Type 'help' to list command groups, or a group name to list "
                "its commands.\n\n");
}

/*
 * Probe the attached terminal for escape-sequence support and greet it.
 *
 * Called once the host is known to be attached, and again after a reconnect:
 * on USB-Serial-JTAG the board typically boots with nothing plugged in, and a
 * probe sent into the void would strand the shell in dumb mode for the rest of
 * the session.
 */
static void greet_terminal(void)
{
    linenoiseSetDumbMode(0);
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }

    print_banner();

    if (linenoiseIsDumbMode()) {
        diag_printf("Terminal does not support escape sequences; "
                    "line editing, history and completion are disabled.\n\n");
    }
}

static void reader_task(void *arg)
{
    (void)arg;

    linenoiseSetMultiLine(1);
    linenoiseSetMaxLineLen(MAX_LINE_LEN);
    linenoiseHistorySetMaxLen(CONFIG_CLI_HISTORY_LEN);
    linenoiseSetCompletionCallback(&completion_cb);

    bool greeted = false;

    while (1) {
        if (!cli_io_host_connected()) {
            /* Host went away; re-greet whoever attaches next. */
            greeted = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!greeted) {
            greet_terminal();
            greeted = true;
        }

        char *line = linenoise(cli_prompt());
        if (!line) {
            /* Empty line or read error; just re-prompt. */
            continue;
        }

        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            cli_submit(line, true);
        }

        linenoiseFree(line);
    }
}

esp_err_t cli_start(const cli_config_t *cfg)
{
    if (command_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    banner_text = cfg ? cfg->banner : NULL;

    if (cfg && cfg->prompt) {
        prompt_text = cfg->prompt;
    } else {
        /* Built once. There is no menu path to track, so unlike the nested
         * design this never changes after startup -- it only identifies the
         * chip, which is the part that was actually useful. */
        snprintf(prompt_buf, sizeof(prompt_buf), "%s> ", CONFIG_IDF_TARGET);
        prompt_text = prompt_buf;
    }

    esp_err_t err = diag_init(NULL);
    if (err != ESP_OK) {
        return err;
    }
    cli_io_init();

    command_queue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_item_t));
    if (!command_queue) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(executor_task, "cli_exec", EXECUTOR_STACK, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* The banner is printed by the reader once a terminal is actually attached. */
    if (xTaskCreate(reader_task, "cli_repl", READER_STACK, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
