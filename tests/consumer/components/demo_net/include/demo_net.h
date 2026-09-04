#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "net_config.h"   /* generated; net_config_t is this component's public config type */

#ifdef __cplusplus
extern "C" {
#endif

/* Returns false on success, matching the generated parsers' convention. */
bool demo_net_parse(const char *json, size_t len, net_config_t *out);

#ifdef __cplusplus
}
#endif
