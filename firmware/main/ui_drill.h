#ifndef UI_DRILL_H
#define UI_DRILL_H
#include <stdbool.h>
#include "nem/regions.h"
#include "nem/snapshot.h"

/* Region's net interconnector export in MW (>0 export, <0 import), summed from
 * its per-link flows resolved to the region's perspective — the same figure the
 * interconnector drill tile shows, so callers reconcile with it. */
double ui_drill_net_export(const nem_snapshot_t *snap, nem_region_t region);

/* Open the drill-in overlay for `region` (3 swipeable tiles). Inside lock. */
void ui_drill_show(nem_region_t region);
/* Refresh the visible tile from the latest data; no-op if closed. Inside lock. */
void ui_drill_refresh(void);
bool ui_drill_is_open(void);

#endif
