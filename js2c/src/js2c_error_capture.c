#include "js2c_error_capture.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char s_err[JS2C_ERROR_CAPTURE_SIZE];
static bool s_have_err;

void js2c_error_capture_reset(void) {
  s_err[0] = '\0';
  s_have_err = false;
}

const char *js2c_error_capture_get(void) { return s_err; }

/*
 * js2c error text interpolates the offending token straight out of the caller's
 * document, so it can carry anything the client sent. httpd_resp_send_err sends
 * its message as text/html, which a browser will render — escape the markup
 * characters rather than trusting the client, and flatten control characters,
 * which would otherwise split a log line.
 *
 * Escaping expands, so a message near the buffer limit is truncated at the last
 * whole character rather than cut mid-entity.
 */
static void escape_html(const char *in, char *out, size_t out_size) {
  size_t w = 0;
  for (const char *r = in; *r; ++r) {
    unsigned char c = (unsigned char)*r;
    const char *rep;

    switch (c) {
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '&':  rep = "&amp;";  break;
      case '"':  rep = "&quot;"; break;
      default:   rep = NULL;     break;
    }

    if (rep) {
      size_t n = strlen(rep);
      if (w + n >= out_size) {
        break;
      }
      memcpy(out + w, rep, n);
      w += n;
    } else {
      if (w + 1 >= out_size) {
        break;
      }
      out[w++] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
    }
  }
  out[w] = '\0';
}

void js2c_error_capture_record(int position, const char *fmt, ...) {
  /*
   * First error wins: js2c reports the innermost failure, then returns up
   * through enclosing parsers that may log vaguer messages on the way out.
   */
  if (s_have_err) {
    return;
  }
  s_have_err = true;

  char raw[JS2C_ERROR_CAPTURE_SIZE];

  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(raw, sizeof(raw), fmt, args);
  va_end(args);

  if (n < 0) {
    s_err[0] = '\0';
    return;
  }

  /* Append the byte offset before escaping, so it is escaped in one pass. */
  size_t len = strlen(raw);
  if (position >= 0 && len + 1 < sizeof(raw)) {
    snprintf(raw + len, sizeof(raw) - len, " (at byte %d)", position);
  }

  escape_html(raw, s_err, sizeof(s_err));
}
