#include "demo_net.h"

bool demo_net_parse(const char *json, size_t len, net_config_t *out) {
    return json_parse_net_config_with_len(json, len, out);
}
