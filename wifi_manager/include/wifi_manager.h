#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

/*
 * WiFi station and access point: scanning, prioritised known networks, connection
 * timeouts, reconnect backoff and an optional reachability check.
 *
 * State changes are published as NET_EVENT (net_events.h) rather than by calling
 * into the services that care, so the dependency graph only points downward.
 */

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "ping/ping_sock.h"
#include "wifi_manager_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_STA_DISCONNECTED,
    WIFI_MANAGER_STA_CONNECTING,
    WIFI_MANAGER_STA_CONNECTED,
    WIFI_MANAGER_AP_MODE,
    WIFI_MANAGER_POWERED_OFF,
} wifi_manager_state_t;

/*
 * Set up the driver and the reconnection machinery. Does not connect.
 *
 * Nothing that runs *over* the link is started here. mDNS, SNTP and MQTT
 * subscribe to NET_EVENT (net_events.h) and start themselves when there is
 * something to run over, so this builds without them and they work without WiFi
 * -- an Ethernet driver posting the same events serves them equally.
 */
esp_err_t wifi_manager_init(const wifi_manager_config_t *wifi_cfg);
esp_err_t wifi_manager_deinit(void);

esp_err_t wifi_manager_start_station_mode(void);
esp_err_t wifi_manager_start_ap_mode(void);
esp_err_t wifi_manager_stop(void);

esp_err_t wifi_manager_add_known_network(const char *ssid, const char *password);
esp_err_t wifi_manager_remove_known_network(const char *ssid);

esp_err_t wifi_manager_scan_and_connect(void);

/*
 * Scan and hand the results to the caller, for a user interface that wants to
 * show what is nearby rather than join it.
 *
 * This exists because the caller cannot do it alone. A scan started with
 * esp_wifi_scan_start() outside this component races the manager's own
 * WIFI_EVENT_SCAN_DONE handler, which either consumes the records to look for a
 * known network or clears them to release driver memory -- and it wins, every
 * time, so the caller's blocking scan returns an empty list while the manager
 * logs the access points it just found. Scanning has to be done by whoever owns
 * the event handler.
 *
 * Blocks for the duration of the scan, a second or two. `*count` is set to the
 * number of records written, at most `max`.
 *
 * The manager's own use of this scan is given up while it runs: if it was
 * looking for a known network to join, that attempt is skipped and the reconnect
 * timer tries again. A scan is cheap and the retry is already there; the
 * alternative is copying every record on the chance someone asks for them.
 *
 * ESP_ERR_INVALID_STATE if the driver is not up, the radio is in AP mode or
 * powered off, or a scan for a caller is already running.
 */
esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max,
                            uint16_t *count);
wifi_manager_state_t wifi_manager_get_state(void);
esp_err_t wifi_manager_get_rssi(int8_t *rssi);
esp_err_t wifi_manager_get_address(char *dest, size_t length);

bool wifi_manager_has_clients(void);
esp_err_t wifi_manager_force_disconnect(void);

void wifi_manager_record_reachability_result(bool is_reachable);
uint32_t wifi_manager_consecutive_reachability_failures(void);

/* Time synchronisation moved to ntp_manager, which owns the clock and posts
 * NET_EVENT_TIME_SYNCED. It lived here only because this file happened to be
 * where esp_netif_sntp was initialised. */

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */