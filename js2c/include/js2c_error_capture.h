/*
 * Captures the error messages that json_schema_to_c parsers emit, so callers
 * can report *why* a document was rejected instead of a flat "invalid JSON".
 *
 * js2c reports errors through the LOG_ERROR macro, which js2c_builtins.h
 * leaves empty unless something defines it first. Every diagnostic the
 * generated parsers produce — out-of-range integers, unknown fields, duplicate
 * keys, type mismatches — is therefore discarded by default.
 *
 * Because LOG_ERROR is resolved at compile time inside the *generated* parser's
 * translation unit, this header has to be in scope before js2c_builtins.h is
 * included there. The generated .c files are overwritten on every build, so
 * they cannot include it themselves; components that want capture force-include
 * this header instead:
 *
 *     target_compile_options(${COMPONENT_LIB} PRIVATE -include js2c_error_capture.h)
 *
 * Usage around a parse:
 *
 *     js2c_error_capture_reset();
 *     if (json_parse_foo(body, &out)) {
 *         const char *why = js2c_error_capture_get();
 *     }
 *
 * The capture buffer is a single static, so a parse whose errors you intend to
 * read must not be interleaved with another on a different task. Reading it
 * from the esp_http_server task is safe: that task runs handlers one at a time.
 */
#ifndef JS2C_ERROR_CAPTURE_H
#define JS2C_ERROR_CAPTURE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Long enough for a diagnostic plus the offending token js2c quotes back. */
#define JS2C_ERROR_CAPTURE_SIZE 192

/* Arm the buffer. Call immediately before each parse whose error you want. */
void js2c_error_capture_reset(void);

/*
 * The first error from the most recent parse, or "" if none was recorded.
 * First rather than last: js2c reports the innermost failure first, then may
 * add less specific context as it unwinds.
 *
 * Flattened to printable ASCII with markup characters HTML-escaped: the text
 * quotes back caller-supplied bytes, and httpd_resp_send_err sends its message
 * as text/html.
 */
const char *js2c_error_capture_get(void);

/* Backs the LOG_ERROR macro below; not called directly. */
void js2c_error_capture_record(int position, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

/*
 * Must win the #ifndef LOG_ERROR in js2c_builtins.h. Guarded so a translation
 * unit that already defined its own LOG_ERROR (the host tests do) keeps it.
 */
#ifndef LOG_ERROR
#define LOG_ERROR(position, ...) js2c_error_capture_record((int)(position), __VA_ARGS__)
#endif

#endif /* JS2C_ERROR_CAPTURE_H */
