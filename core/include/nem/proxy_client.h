#ifndef NEM_PROXY_CLIENT_H
#define NEM_PROXY_CLIENT_H

#include <stdbool.h>
#include "nem/snapshot.h"
#include "nem/fuel.h"
#include "nem/history.h"

/* Parse the nem-buddy proxy's compact JSON into a snapshot (price/demand) and
 * per-region fuel mix. Zeroes both outputs; marks parsed regions valid.
 * Returns true if at least one region was parsed. */
bool nem_proxy_parse(const char *json, nem_snapshot_t *snap, nem_region_mix_t *mix);

/* Parse the proxy's per-region intraday curves ("ph"/"dh": comma-separated
 * price/demand per 5-min slot of day, index == slot) into `hist`. Re-inits
 * `hist` first. Returns true if at least one region's curve was parsed. */
bool nem_proxy_parse_history(const char *json, nem_history_t *hist);

#endif
