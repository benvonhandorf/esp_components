#include "int_dispatch.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

static const char *TAG = "int_dispatch";

#define MAX_HANDLERS   CONFIG_INT_DISPATCH_MAX_HANDLERS
#define DEFAULT_STACK  3072
#define DEFAULT_PRIO   5

typedef struct {
    int_dispatch_cb_t cb;
    void *ctx;
} handler_t;

static handler_t s_handlers[MAX_HANDLERS];
static size_t s_handler_count;
static TaskHandle_t s_task;
static gpio_num_t s_pin = GPIO_NUM_NC;
static volatile uint32_t s_unserviced;

/* Runs in interrupt context: nothing here may block or touch a driver. */
static void IRAM_ATTR gpio_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void dispatch_task(void *arg)
{
    (void)arg;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /*
         * The line is wired-AND, so an edge says only that something asserted
         * it. Every handler runs and checks its own status register; there is no
         * way to know which part it was without asking.
         */
        for (size_t i = 0; i < s_handler_count; i++) {
            s_handlers[i].cb(s_handlers[i].ctx);
        }

        /*
         * Still low: at least one part has an interrupt nobody cleared. Re-arm
         * immediately, because the line is level-triggered in effect and a
         * further edge will never come -- without this the device simply stops
         * responding to that part.
         *
         * Counted, because a rising count is the only sign that a handler is not
         * clearing what it should. Left uncounted this is a busy loop that just
         * makes the device feel slow.
         */
        if (gpio_get_level(s_pin) == 0) {
            s_unserviced++;
            xTaskNotifyGive(s_task);
        }
    }
}

esp_err_t int_dispatch_start(const int_dispatch_config_t *cfg)
{
    if (!cfg || !GPIO_IS_VALID_GPIO(cfg->pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pin = cfg->pin;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = cfg->pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    /* Created before the ISR is installed, so the handle the ISR notifies is
     * valid the first time the line is asserted. */
    if (xTaskCreate(dispatch_task, "int_dispatch",
                    cfg->task_stack ? cfg->task_stack : DEFAULT_STACK, NULL,
                    cfg->task_priority ? cfg->task_priority : DEFAULT_PRIO,
                    &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The ISR service is process-wide, so a project that installed it elsewhere
     * is not an error. This is the one piece of state here that is genuinely not
     * per-instance.
     */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }

    err = gpio_isr_handler_add(cfg->pin, gpio_isr, NULL);
    if (err != ESP_OK) {
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }

    ESP_LOGI(TAG, "dispatching interrupts from GPIO %d", (int)cfg->pin);

    /* A part may already be asserting the line, in which case the edge has
     * happened and will not repeat. Check once at startup. */
    if (gpio_get_level(cfg->pin) == 0) {
        xTaskNotifyGive(s_task);
    }

    return ESP_OK;
}

esp_err_t int_dispatch_stop(void)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_isr_handler_remove(s_pin);
    vTaskDelete(s_task);
    s_task = NULL;
    return ESP_OK;
}

esp_err_t int_dispatch_register(int_dispatch_cb_t cb, void *ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_handler_count == MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    s_handlers[s_handler_count].cb = cb;
    s_handlers[s_handler_count].ctx = ctx;
    s_handler_count++;
    return ESP_OK;
}

uint32_t int_dispatch_unserviced_count(void)
{
    return s_unserviced;
}
