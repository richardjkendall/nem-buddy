#ifndef UI_DRILL_H
#define UI_DRILL_H
#include <stdbool.h>
#include "nem/regions.h"

/* Open the drill-in overlay for `region` (3 swipeable tiles). Inside lock. */
void ui_drill_show(nem_region_t region);
/* Refresh the visible tile from the latest data; no-op if closed. Inside lock. */
void ui_drill_refresh(void);
bool ui_drill_is_open(void);

#endif
