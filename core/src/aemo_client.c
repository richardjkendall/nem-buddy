#include "nem/aemo_client.h"
#include "nem/regions.h"
#include "nem/timeutil.h"
#include "cJSON.h"
#include <string.h>

static double num(const cJSON *obj, const char *key) {
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) ? n->valuedouble : 0.0;
}

static void parse_flows(const char *flows_json, nem_region_snapshot_t *r) {
    r->interconnector_count = 0;
    if (!flows_json) return;
    cJSON *arr = cJSON_Parse(flows_json);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        if (r->interconnector_count >= NEM_MAX_INTERCONNECTORS) break;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(el, "name");
        nem_interconnector_flow_t *f = &r->interconnectors[r->interconnector_count];
        if (cJSON_IsString(name) && name->valuestring) {
            strncpy(f->name, name->valuestring, sizeof(f->name) - 1);
            f->name[sizeof(f->name) - 1] = 0;
        } else {
            f->name[0] = 0;
        }
        f->value = num(el, "value");
        r->interconnector_count++;
    }
    cJSON_Delete(arr);
}

bool nem_aemo_parse_summary(const char *json, nem_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < NEM_REGION_COUNT; i++) out->regions[i].region = (nem_region_t)i;
    if (!json) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "ELEC_NEM_SUMMARY");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return false; }

    int parsed = 0;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(el, "REGIONID");
        if (!cJSON_IsString(rid)) continue;
        nem_region_t reg = nem_region_from_id(rid->valuestring);
        if (reg >= NEM_REGION_COUNT) continue;

        nem_region_snapshot_t *r = &out->regions[reg];
        r->valid = true;
        r->price = num(el, "PRICE");
        r->demand_mw = num(el, "TOTALDEMAND");
        r->net_interchange = num(el, "NETINTERCHANGE");

        const cJSON *sd = cJSON_GetObjectItemCaseSensitive(el, "SETTLEMENTDATE");
        r->settlement_epoch = 0;
        if (cJSON_IsString(sd)) nem_parse_iso8601(sd->valuestring, &r->settlement_epoch);

        const cJSON *fl = cJSON_GetObjectItemCaseSensitive(el, "INTERCONNECTORFLOWS");
        parse_flows(cJSON_IsString(fl) ? fl->valuestring : NULL, r);
        parsed++;
    }
    cJSON_Delete(root);
    return parsed > 0;
}
