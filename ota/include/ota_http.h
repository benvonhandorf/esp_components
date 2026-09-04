#ifndef OTA_HTTP_H
#define OTA_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An HTTP route that accepts a firmware image as the body of a POST.
 *
 *   curl -X POST --data-binary @build/firmware.bin \
 *        -u admin:secret http://device.local/api/ota
 *
 * Registered through http_server, so it is authenticated like any other
 * protected route rather than carrying its own credential check. The transport
 * is separate from ota.h on purpose: an update delivered over MQTT or a serial
 * console uses the session API directly.
 */

/* Register the route. `uri` may be NULL for "/api/ota". The device reboots into
 * the new image shortly after responding, so the response goes out first. */
esp_err_t ota_http_register(const char *uri);

#ifdef __cplusplus
}
#endif

#endif /* OTA_HTTP_H */
