#ifndef HTTP_AUTH_H
#define HTTP_AUTH_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Basic-auth credential handling, kept apart from the server so it can be tested
 * on the host. Credential checks are the last place to find out a subtle bug on
 * hardware.
 */

/*
 * Base64-encode `in` into `out`. Returns false if it would not fit.
 *
 * This encodes our *own* credentials, never anything from the network -- see
 * http_auth_header_matches() for why that direction was chosen.
 */
bool http_auth_base64(const char *in, size_t in_len, char *out, size_t out_size);

/*
 * Build the expected value of an Authorization header, i.e.
 * "Basic " + base64("user:pass"). Returns false if it would not fit.
 */
bool http_auth_expected_header(const char *user, const char *password,
                               char *out, size_t out_size);

/*
 * Whether an Authorization header matches the expected value.
 *
 * Compares the *encoded* forms rather than decoding what the client sent. That
 * is deliberate: it means no attacker-controlled Base64 is ever parsed, which
 * removes a decoder -- and its handling of invalid characters, bad padding and
 * short groups -- from the attack surface entirely. Base64 has one canonical
 * encoding per input, and RFC 7617 requires clients to use it, so comparing
 * encodings is equivalent to comparing credentials.
 *
 * The comparison runs in time independent of how far it matches, so the number
 * of correct leading characters cannot be recovered by timing repeated requests.
 */
bool http_auth_header_matches(const char *header, const char *expected);

#endif /* HTTP_AUTH_H */
