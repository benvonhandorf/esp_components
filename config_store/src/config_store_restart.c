#include "config_store.h"

#include "esp_system.h"
#include "esp_timer.h"

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

esp_err_t config_store_schedule_restart(uint32_t delay_ms)
{
    /* One-shot, and deliberately not cancellable: the caller has already decided
     * to reboot, and a second call simply schedules a second timer that never
     * gets the chance to fire. */
    const esp_timer_create_args_t args = {
        .callback = restart_cb,
        .name = "cfg_restart",
        .dispatch_method = ESP_TIMER_TASK,
    };

    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
}
