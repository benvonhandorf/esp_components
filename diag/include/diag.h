#ifndef DIAG_H
#define DIAG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Command and log output fan-out.
 *
 * A device with more than one operator interface has to show the same text on
 * all of them: output from a command typed on the serial port must also reach a
 * browser attached over a WebSocket, and vice versa. Modules therefore never
 * call printf() directly -- they call diag_printf(), which writes to stdout
 * *and* to every registered sink.
 *
 * This is deliberately separate from the `cli` component. The fan-out is what
 * makes logging to a filesystem, to MQTT or to a web page possible, and a
 * headless project wants that without a command shell. diag depends on nothing.
 *
 * Sinks live with their transport, not here: an MQTT log sink belongs to the
 * MQTT component, a file sink to whatever owns the filesystem. That is what
 * keeps this component's dependency list empty.
 */

/*
 * Receives a chunk of already-formatted output.
 *
 * The fan-out holds its output lock across this call, and it is called from
 * whatever task produced the line. Two rules follow from that, and both have
 * been learned the hard way:
 *
 * 1. Do not call diag_printf() or anything else in this header. Re-entering the
 *    fan-out recurses until the stack is gone.
 *
 * 2. Do not block, and in particular do not call into another subsystem and
 *    wait for it. The caller may be *any* task, including lwIP's TCP/IP thread:
 *    ESP_LOGx from an lwIP raw-API callback -- the SNTP time-sync notification
 *    is one -- arrives here on that thread, and a socket call made from it
 *    posts to the TCP/IP mailbox and waits for the thread that is already
 *    inside this function. That deadlocks permanently, with the output lock
 *    held, which silently kills every interface on the device.
 *
 * A sink whose transport can block must hand the text to its own task and
 * return: a bounded queue with a zero-tick send, dropping and counting on
 * overflow. See cli_web and mqtt_log_sink for the shape.
 */
typedef void (*diag_sink_fn)(const char *text, size_t len, void *ctx);

typedef struct {
    /*
     * Route ESP_LOGx through the same fan-out, so log lines reach every
     * interface rather than only the serial port. Default true.
     */
    bool capture_esp_log;
} diag_config_t;

/* Set up the output lock and, unless disabled, the ESP_LOGx redirect.
 * `cfg` may be NULL for defaults. Safe to call more than once. */
esp_err_t diag_init(const diag_config_t *cfg);

/* ESP_ERR_NO_MEM if the sink table is full (CONFIG_DIAG_MAX_SINKS). */
esp_err_t diag_sink_register(diag_sink_fn fn, void *ctx);
esp_err_t diag_sink_unregister(diag_sink_fn fn, void *ctx);

/* printf() for anything a person or a script is meant to read. */
void diag_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_vprintf(const char *fmt, va_list args);

/* Emit a pre-formatted buffer without going through printf formatting. */
void diag_write(const char *text, size_t len);

/*
 * Report a failure in a uniform, machine-greppable way: the line is prefixed
 * with "ERR: " so a host-side script driving the device can tell success from
 * failure without parsing prose. Appends its own newline.
 */
void diag_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* DIAG_H */
