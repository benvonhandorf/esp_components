#include "mdns_manager.h"

#include <string.h>

#include "esp_log.h"
#include "mdns.h"
#include "net_events.h"

#include "sdkconfig.h"

static const char *TAG = "mdns_manager";

#define MAX_SERVICES CONFIG_MDNS_MANAGER_MAX_SERVICES
#define MAX_HOSTNAME 64
#define MAX_INSTANCE 64
#define MAX_TYPE     32
#define MAX_TXT_STR  64

/*
 * Services are copied rather than referenced. A caller registering from a stack
 * frame, or from a config struct it later reloads, would otherwise leave the
 * table pointing at freed or changed memory -- and the table has to survive to
 * be re-advertised after every reconnect.
 */
typedef struct {
    char type[MAX_TYPE];
    char proto[8];
    uint16_t port;
    char instance[MAX_INSTANCE];
    mdns_txt_item_t txt[MDNS_MANAGER_MAX_TXT];
    char txt_storage[MDNS_MANAGER_MAX_TXT][2][MAX_TXT_STR];
    size_t txt_count;
} service_entry_t;

static service_entry_t s_services[MAX_SERVICES];
static size_t s_service_count;

static char s_hostname[MAX_HOSTNAME];
static char s_instance[MAX_INSTANCE];
static bool s_started;      /* subscribed and mdns_init() done */
static bool s_advertising;  /* hostname and services published */

static void copy_str(char *dest, size_t dest_size, const char *src)
{
    if (!src) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static esp_err_t publish_service(service_entry_t *entry)
{
    const char *instance = entry->instance[0] ? entry->instance : NULL;

    esp_err_t err = mdns_service_add(instance, entry->type, entry->proto,
                                     entry->port,
                                     entry->txt_count ? entry->txt : NULL,
                                     entry->txt_count);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "advertising %s%s: %s", entry->type, entry->proto,
                 esp_err_to_name(err));
    }
    return err;
}

static void publish_all(void)
{
    if (s_hostname[0] == '\0') {
        ESP_LOGW(TAG, "no hostname configured; not advertising");
        return;
    }

    esp_err_t err = mdns_hostname_set(s_hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setting hostname: %s", esp_err_to_name(err));
        return;
    }
    if (s_instance[0] != '\0') {
        mdns_instance_name_set(s_instance);
    }

    for (size_t i = 0; i < s_service_count; i++) {
        publish_service(&s_services[i]);
    }

    s_advertising = true;
    ESP_LOGI(TAG, "advertising %s.local with %u service(s)",
             s_hostname, (unsigned)s_service_count);
}

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch ((net_event_id_t)id) {
        case NET_EVENT_LINK_UP:
        case NET_EVENT_AP_STARTED:
            /*
             * Advertise on an access point too: a device that found no known
             * network is exactly the one a person needs to reach by name, since
             * there is no router to ask for its address.
             */
            publish_all();
            break;
        case NET_EVENT_LINK_DOWN:
        case NET_EVENT_AP_STOPPED:
            s_advertising = false;
            break;
        default:
            break;
    }
}

esp_err_t mdns_manager_start(const mdns_manager_config_t *cfg)
{
    if (!cfg || !cfg->hostname || cfg->hostname[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    copy_str(s_hostname, sizeof(s_hostname), cfg->hostname);
    copy_str(s_instance, sizeof(s_instance), cfg->instance_name);

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, &on_net_event, NULL);
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "ready; will advertise %s.local once the network is up", s_hostname);
    return ESP_OK;
}

esp_err_t mdns_manager_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_event_handler_unregister(NET_EVENT, ESP_EVENT_ANY_ID, &on_net_event);
    mdns_free();

    s_started = false;
    s_advertising = false;
    return ESP_OK;
}

esp_err_t mdns_manager_add_service(const mdns_manager_service_t *service)
{
    if (!service || !service->type || !service->proto) {
        return ESP_ERR_INVALID_ARG;
    }
    if (service->txt_count > MDNS_MANAGER_MAX_TXT) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_service_count == MAX_SERVICES) {
        return ESP_ERR_NO_MEM;
    }

    service_entry_t *entry = &s_services[s_service_count];
    memset(entry, 0, sizeof(*entry));

    copy_str(entry->type, sizeof(entry->type), service->type);
    copy_str(entry->proto, sizeof(entry->proto), service->proto);
    copy_str(entry->instance, sizeof(entry->instance), service->instance);
    entry->port = service->port;

    for (size_t i = 0; i < service->txt_count; i++) {
        copy_str(entry->txt_storage[i][0], MAX_TXT_STR, service->txt[i].key);
        copy_str(entry->txt_storage[i][1], MAX_TXT_STR, service->txt[i].value);
        entry->txt[i].key = entry->txt_storage[i][0];
        entry->txt[i].value = entry->txt_storage[i][1];
    }
    entry->txt_count = service->txt_count;

    s_service_count++;

    /* Registered after the link came up: publish it now rather than making the
     * caller wait for a reconnect. */
    if (s_advertising) {
        publish_service(entry);
    }

    return ESP_OK;
}

bool mdns_manager_is_advertising(void)
{
    return s_advertising;
}
