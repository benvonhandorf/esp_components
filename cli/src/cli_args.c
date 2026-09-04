#include "cli.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * Argument parsing for command implementations.
 *
 * Lifted from esp_board_bringup's esp_bringup.c. Names gained a cli_ prefix on
 * the way in: a component's public header must not put bare `parse_int_arg` in
 * every consumer's namespace.
 */

/* Guard rail so a typo like "0-100000" cannot exhaust the heap. */
#define MAX_PINS 64
/* Parse an integer that must consume its entire token. */
static int parse_in_base(const char *token, int base, int *out)
{
    if (!token) {
        return -1;
    }
    while (*token == ' ') {
        token++;
    }
    if (*token == '\0') {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(token, &end, base);

    if (errno != 0 || end == token) {
        return -1;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0') {
        return -1; /* trailing garbage, e.g. "4x" */
    }
    if (value < INT_MIN || value > INT_MAX) {
        return -1;
    }

    *out = (int)value;
    return 0;
}

int cli_parse_int_arg(const char *token, int *out)
{
    return parse_in_base(token, 10, out);
}

int cli_parse_num_arg(const char *token, int *out)
{
    const char *digits = token;
    while (digits && *digits == ' ') {
        digits++;
    }

    bool negative = digits && *digits == '-';
    if (negative) {
        digits++;
    }

    if (digits && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        int value = 0;
        if (parse_in_base(digits + 2, 16, &value) < 0) {
            return -1;
        }
        *out = negative ? -value : value;
        return 0;
    }

    return parse_in_base(token, 10, out);
}

int cli_parse_double_arg(const char *token, double *out)
{
    if (!token) {
        return -1;
    }
    while (*token == ' ') {
        token++;
    }
    if (*token == '\0') {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    double value = strtod(token, &end);

    if (errno != 0 || end == token) {
        return -1;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0') {
        return -1; /* trailing garbage, e.g. "0.004ohm" */
    }
    if (!isfinite(value)) {
        return -1;
    }

    *out = value;
    return 0;
}

static int append_pin(int **pins, int *count, int pin)
{
    if (*count >= MAX_PINS) {
        return -1;
    }

    int *grown = realloc(*pins, (size_t)(*count + 1) * sizeof(int));
    if (!grown) {
        return -1;
    }

    *pins = grown;
    (*pins)[(*count)++] = pin;
    return 0;
}

int cli_parse_pin_list(const char *pin_str, int **pins, int *count)
{
    *pins = NULL;
    *count = 0;

    if (!pin_str || *pin_str == '\0') {
        return -1;
    }

    char *str = strdup(pin_str);
    if (!str) {
        return -1;
    }

    int result = 0;
    char *save = NULL;

    for (char *token = strtok_r(str, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
        /* A '-' at position 0 would be a negative number, not a range. */
        char *dash = strchr(token + (token[0] == '-' ? 1 : 0), '-');

        if (dash) {
            *dash = '\0';
            int start = 0;
            int end = 0;
            if (cli_parse_int_arg(token, &start) < 0 || cli_parse_int_arg(dash + 1, &end) < 0) {
                result = -1;
                break;
            }
            if (end < start) {
                result = -1;
                break;
            }
            for (int pin = start; pin <= end; pin++) {
                if (append_pin(pins, count, pin) < 0) {
                    result = -1;
                    break;
                }
            }
            if (result < 0) {
                break;
            }
        } else {
            int pin = 0;
            if (cli_parse_int_arg(token, &pin) < 0 || append_pin(pins, count, pin) < 0) {
                result = -1;
                break;
            }
        }
    }

    free(str);

    if (result < 0 || *count == 0) {
        free(*pins);
        *pins = NULL;
        *count = 0;
        return -1;
    }

    return 0;
}
