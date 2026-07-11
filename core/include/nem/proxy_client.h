#ifndef NEM_PROXY_CLIENT_H
#define NEM_PROXY_CLIENT_H

#include <stdbool.h>
#include "nem/snapshot.h"
#include "nem/fuel.h"

/* Parse the nem-buddy proxy's compact JSON into a snapshot (price/demand) and
 * per-region fuel mix. Zeroes both outputs; marks parsed regions valid.
 * Returns true if at least one region was parsed. */
bool nem_proxy_parse(const char *json, nem_snapshot_t *snap, nem_region_mix_t *mix);

#endif
