#include "mqtt_topic.h"

#include <string.h>

bool mqtt_topic_matches(const char *filter, const char *topic, size_t topic_len)
{
    if (!filter || !topic) {
        return false;
    }

    size_t f = 0;
    size_t t = 0;
    const size_t flen = strlen(filter);

    while (f < flen && t < topic_len) {
        if (filter[f] == '#') {
            /* Valid only as the final character, and only as a whole level. It
             * matches the rest of the topic including no levels at all. */
            return (f == flen - 1) && (f == 0 || filter[f - 1] == '/');
        }

        if (filter[f] == '+') {
            /* One level, and the *whole* level: it must stand alone between
             * separators. Checking only what follows would let "a+/x" match
             * "ab/x", turning a malformed filter into a prefix match. */
            if (f > 0 && filter[f - 1] != '/') {
                return false;
            }
            if (f + 1 < flen && filter[f + 1] != '/') {
                return false;
            }
            while (t < topic_len && topic[t] != '/') {
                t++;
            }
            f++;
            /* Both sides now sit on a separator, or both are exhausted. */
            if (f < flen && t < topic_len && filter[f] == '/' && topic[t] == '/') {
                f++;
                t++;
            }
            continue;
        }

        if (filter[f] != topic[t]) {
            return false;
        }
        f++;
        t++;
    }

    /* "a/b/#" matches "a/b" exactly: '#' covers the parent level too. */
    if (t == topic_len && flen - f == 2 && filter[f] == '/' && filter[f + 1] == '#') {
        return true;
    }

    return f == flen && t == topic_len;
}

/* Append at most what is left, reporting failure rather than truncating: a
 * truncated topic is a topic that silently goes somewhere else. */
static bool append(char *out, size_t out_size, size_t *used, const char *text,
                   size_t len)
{
    if (*used + len + 1 > out_size) {
        return false;
    }
    memcpy(out + *used, text, len);
    *used += len;
    out[*used] = '\0';
    return true;
}

bool mqtt_topic_build(char *out, size_t out_size, const char *prefix,
                      const char *device_name, const char *suffix)
{
    static const char TOKEN[] = "$DEVICE$";
    const size_t token_len = sizeof(TOKEN) - 1;

    if (!out || out_size == 0) {
        return false;
    }

    size_t used = 0;
    out[0] = '\0';

    const char *p = prefix ? prefix : "";
    while (*p) {
        const char *hit = strstr(p, TOKEN);
        if (!hit) {
            if (!append(out, out_size, &used, p, strlen(p))) {
                return false;
            }
            break;
        }
        if (!append(out, out_size, &used, p, (size_t)(hit - p))) {
            return false;
        }
        const char *name = device_name ? device_name : "";
        if (!append(out, out_size, &used, name, strlen(name))) {
            return false;
        }
        p = hit + token_len;
    }

    if (!suffix || suffix[0] == '\0') {
        return true;
    }

    bool prefix_ends_slash = (used > 0 && out[used - 1] == '/');
    bool suffix_starts_slash = (suffix[0] == '/');

    if (prefix_ends_slash && suffix_starts_slash) {
        suffix++;   /* would double the separator */
    } else if (used > 0 && !prefix_ends_slash && !suffix_starts_slash) {
        if (!append(out, out_size, &used, "/", 1)) {
            return false;
        }
    }

    return append(out, out_size, &used, suffix, strlen(suffix));
}

bool mqtt_topic_needs_device(const char *prefix)
{
    return prefix && strstr(prefix, "$DEVICE$") != NULL;
}
