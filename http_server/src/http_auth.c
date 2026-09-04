#include "http_auth.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool http_auth_base64(const char *in, size_t in_len, char *out, size_t out_size)
{
    if (!in || !out) {
        return false;
    }

    const size_t encoded_len = ((in_len + 2) / 3) * 4;
    if (encoded_len + 1 > out_size) {
        return false;
    }

    size_t o = 0;
    size_t i = 0;

    while (i + 2 < in_len) {
        uint32_t v = ((uint32_t)(unsigned char)in[i] << 16) |
                     ((uint32_t)(unsigned char)in[i + 1] << 8) |
                     ((uint32_t)(unsigned char)in[i + 2]);
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = B64[(v >> 6) & 0x3f];
        out[o++] = B64[v & 0x3f];
        i += 3;
    }

    const size_t remaining = in_len - i;
    if (remaining == 1) {
        uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = '=';
        out[o++] = '=';
    } else if (remaining == 2) {
        uint32_t v = ((uint32_t)(unsigned char)in[i] << 16) |
                     ((uint32_t)(unsigned char)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = B64[(v >> 6) & 0x3f];
        out[o++] = '=';
    }

    out[o] = '\0';
    return true;
}

bool http_auth_expected_header(const char *user, const char *password,
                               char *out, size_t out_size)
{
    if (!user || !password || !out) {
        return false;
    }

    char plain[128];
    int n = snprintf(plain, sizeof(plain), "%s:%s", user, password);
    if (n < 0 || (size_t)n >= sizeof(plain)) {
        return false;
    }

    static const char PREFIX[] = "Basic ";
    const size_t prefix_len = sizeof(PREFIX) - 1;
    if (prefix_len >= out_size) {
        return false;
    }
    memcpy(out, PREFIX, prefix_len);

    bool ok = http_auth_base64(plain, (size_t)n, out + prefix_len,
                               out_size - prefix_len);

    /* The plaintext credentials do not outlive this function. */
    memset(plain, 0, sizeof(plain));
    return ok;
}

bool http_auth_header_matches(const char *header, const char *expected)
{
    if (!header || !expected) {
        return false;
    }

    /*
     * Compare every byte of the expected value regardless of where the first
     * difference is, so the time taken does not reveal how much of the
     * credential was correct. Length is compared the same way.
     */
    const size_t expected_len = strlen(expected);
    const size_t header_len = strlen(header);

    unsigned char diff = (unsigned char)((expected_len ^ header_len) != 0);

    for (size_t i = 0; i < expected_len; i++) {
        /* Reading header[i] past its end would be out of bounds, so clamp the
         * index and let the length difference above carry the mismatch. */
        const char h = (i < header_len) ? header[i] : '\0';
        diff |= (unsigned char)(h ^ expected[i]);
    }

    return diff == 0;
}
