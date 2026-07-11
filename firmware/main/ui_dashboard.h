#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H
#include "lvgl.h"
#include "nem/snapshot.h"
#include "nem/regions.h"
#include "nem/fuel.h"

void ui_dashboard_create(lv_obj_t *parent);
/* Update live values for the home region. `mix` may be NULL. Call inside
 * bsp_display_lock()/unlock(). */
void ui_dashboard_update(const nem_snapshot_t *snap, const nem_fuel_mix_t *mix, nem_region_t home);

#endif
