#include "nem/proxy_client.h"
#include "nem/regions.h"
#include "nem/timeutil.h"
#include "nem/history.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

    long long epoch = 0;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(t)) nem_parse_iso8601(t->valuestring, &epoch);

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
        rs->settlement_epoch = epoch;
        rs->net_interchange = num(el, "ni");
        rs->interconnector_count = 0;
        const cJSON *ic = cJSON_GetObjectItemCaseSensitive(el, "ic");
        if (cJSON_IsArray(ic)) {
            const cJSON *pair = NULL;
            cJSON_ArrayForEach(pair, ic) {
                if (rs->interconnector_count >= NEM_MAX_INTERCONNECTORS) break;
                const cJSON *nm = cJSON_GetArrayItem(pair, 0);
                const cJSON *vl = cJSON_GetArrayItem(pair, 1);
                if (!cJSON_IsString(nm) || !cJSON_IsNumber(vl)) continue;
                nem_interconnector_flow_t *f = &rs->interconnectors[rs->interconnector_count++];
                snprintf(f->name, sizeof f->name, "%s", nm->valuestring);
                f->value = vl->valuedouble;
            }
        }

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

/* Parse a comma-separated curve ("35.8,,36.1,...") into vals[]; empty token =
 * gap. When `filled` is non-NULL, marks each present slot. Index == slot. */
static void parse_curve(const char *csv, double *vals, bool *filled, int cap)
{
    if (!csv) return;
    const char *p = csv;
    int idx = 0;
    while (idx < cap) {
        if (*p != ',' && *p != '\0') {
            vals[idx] = strtod(p, NULL);
            if (filled) filled[idx] = true;
        }
        while (*p && *p != ',') p++;
        if (*p == ',') { p++; idx++; } else break;
    }
}

bool nem_proxy_parse_history(const char *json, nem_history_t *hist)
{
    nem_history_init(hist);
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

        const cJSON *ph = cJSON_GetObjectItemCaseSensitive(el, "ph");
        const cJSON *dh = cJSON_GetObjectItemCaseSensitive(el, "dh");
        nem_region_history_t *rh = &hist->regions[reg];
        if (cJSON_IsString(ph)) parse_curve(ph->valuestring, rh->price,  rh->filled, NEM_HISTORY_SLOTS);
        if (cJSON_IsString(dh)) parse_curve(dh->valuestring, rh->demand, NULL,       NEM_HISTORY_SLOTS);
        parsed++;
    }
    cJSON_Delete(root);
    return parsed > 0;
}
