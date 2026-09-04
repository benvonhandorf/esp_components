#ifndef MDNS_MANAGER_H
#define MDNS_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Advertises the device and its services over mDNS, so it can be found by name
 * rather than by whatever address DHCP handed out.
 *
 * Start it at boot, before there is any connectivity: it subscribes to
 * NET_EVENT_LINK_UP and NET_EVENT_AP_STARTED, so no caller has to sequence it
 * against the network coming up. Services registered before the link is up are
 * advertised as soon as it is.
 */

#define MDNS_MANAGER_MAX_TXT 8

typedef struct {
    const char *key;
    const char *value;
} mdns_manager_txt_t;

typedef struct {
    /* Service type without the protocol, e.g. "_http". */
    const char *type;
    /* "_tcp" or "_udp". */
    const char *proto;
    uint16_t port;
    /* Optional instance name; NULL uses the device's instance name. */
    const char *instance;
    /* Optional TXT records, describing what the service actually offers --
     * a path, an API version -- so a client can tell one device's service from
     * another's without connecting to it. */
    const mdns_manager_txt_t *txt;
    size_t txt_count;
} mdns_manager_service_t;

typedef struct {
    /* Hostname without ".local", e.g. "esp-sensor-01". */
    const char *hostname;
    /* Human-readable name shown by browsers, e.g. "Greenhouse Sensor". NULL or
     * empty to leave it unset. */
    const char *instance_name;
} mdns_manager_config_t;

esp_err_t mdns_manager_start(const mdns_manager_config_t *cfg);
esp_err_t mdns_manager_stop(void);

/*
 * Advertise a service.
 *
 * The descriptor is copied, so it may be a temporary. May be called before or
 * after the link is up: services registered early are advertised when it comes
 * up, and the set is re-advertised after a reconnect.
 *
 * This replaces a weak `mdns_manager_register_services()` that applications
 * overrode with a strong symbol. That worked only while everything was compiled
 * into one binary -- the original needed a `volatile` function pointer in main()
 * purely to stop the linker discarding the override -- and it gave a component no
 * way to advertise a service of its own. Registration is data, so anyone can add
 * to it.
 *
 * ESP_ERR_NO_MEM if the table is full (CONFIG_MDNS_MANAGER_MAX_SERVICES).
 */
esp_err_t mdns_manager_add_service(const mdns_manager_service_t *service);

/* Whether the device is currently advertising. */
bool mdns_manager_is_advertising(void);

#ifdef __cplusplus
}
#endif

#endif /* MDNS_MANAGER_H */
