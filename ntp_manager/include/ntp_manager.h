#ifndef NTP_MANAGER_H
#define NTP_MANAGER_H

#include <stdbool.h>
#include <sys/time.h>

#include "esp_err.h"
#include "ntp_config.h"   /* generated from ntp_config_schema.json */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sets the system clock from NTP, once there is a network to do it over.
 *
 * Start it at boot, before there is any connectivity: it subscribes to
 * NET_EVENT_LINK_UP and does nothing until one arrives, so no caller has to
 * sequence it against the network coming up. On success it posts
 * NET_EVENT_TIME_SYNCED, which is what anything timestamping records should wait
 * for -- before that the clock holds whatever the last boot left behind.
 *
 * Deliberately not started on NET_EVENT_AP_STARTED: an access point the device
 * is serving itself has clients but no route to a time server.
 */

esp_err_t ntp_manager_start(const ntp_config_t *cfg);
esp_err_t ntp_manager_stop(void);

/* Whether the clock has been set since boot. */
bool ntp_manager_is_synced(void);

/* Time of the last successful synchronisation, or 0. */
esp_err_t ntp_manager_last_sync(struct timeval *out);

#ifdef __cplusplus
}
#endif

#endif /* NTP_MANAGER_H */
