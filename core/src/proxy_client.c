#include "nem/proxy_client.h"
#include "nem/regions.h"
#include "cJSON.h"
#include <string.h>

/* proxy fuel keys, in nem_fuel_t order (COAL,GAS,HYDRO,WIND,SOLAR,BATTERY,OTHER) */
static const char *const FUEL_KEYS[NEM_FUEL_COUNT] = {
    "coal", "gas", "hydro", "wind", "solar", "battery", "other"
};

static double num(const cJSON *obj, const char *key)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) ? n->valuedouble : 0.0;
}

bool nem_proxy_parse(const char *json, nem_snapshot_t *snap, nem_region_mix_t *mix)
{
    memset(snap, 0, sizeof(*snap));
    memset(mix, 0, sizeof(*mix));
    for (int i = 0; i < NEM_REGION_COUNT; i++) snap->regions[i].region = (nem_region_t)i;
    if (!json) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *regions = cJSON_GetObjectItemCaseSensitive(root, "regions");
    if (!cJSON_IsArray(regions)) { cJSON_Delete(root); return false; }

    int parsed = 0;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, regions) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(el, "id");
        if (!cJSON_IsString(id)) continue;
        nem_region_t reg = nem_region_from_id(id->valuestring);
        if (reg >= NEM_REGION_COUNT) continue;

        nem_region_snapshot_t *rs = &snap->regions[reg];
        rs->valid = true;
        rs->price = num(el, "price");
        rs->demand_mw = num(el, "demand");

        nem_fuel_mix_t *fm = &mix->regions[reg];
        fm->valid = true;
        fm->renewable_fraction = num(el, "ren");
        const cJSON *fuel = cJSON_GetObjectItemCaseSensitive(el, "fuel");
        if (cJSON_IsObject(fuel)) {
            for (int f = 0; f < NEM_FUEL_COUNT; f++) {
                double mw = num(fuel, FUEL_KEYS[f]);
                fm->mw[f] = mw;
                fm->total_mw += mw;
            }
        }
        parsed++;
    }
    cJSON_Delete(root);
    return parsed > 0;
}
