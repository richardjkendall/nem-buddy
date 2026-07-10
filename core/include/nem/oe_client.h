#ifndef NEM_OE_CLIENT_H
#define NEM_OE_CLIENT_H

#include <stdbool.h>
#include "nem/fuel.h"

bool nem_oe_parse_power(const char *json, nem_region_mix_t *out);

#endif
