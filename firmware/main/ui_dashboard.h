#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H
#include "lvgl.h"
#include "nem/snapshot.h"
#include "nem/regions.h"
#include "nem/fuel.h"

void ui_dashboard_create(lv_obj_t *parent);

/* Update from the latest full snapshot + full region mix. Caches both and
 * renders the current hero region. Call inside bsp_display_lock(). */
void ui_dashboard_update(const nem_snapshot_t *snap, const nem_region_mix_t *mix);

/* Current hero region (home by default; changed by ribbon chip taps). */
nem_region_t ui_dashboard_hero_region(void);

/* Latest cached data (may be NULL before the first update). Read under lock. */
const nem_snapshot_t   *ui_dashboard_snapshot(void);
const nem_region_mix_t *ui_dashboard_mix(void);

#endif
