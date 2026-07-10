#include "nem/oe_client.h"
#include "nem/regions.h"
#include "cJSON.h"
#include <string.h>

/* Return the last datapoint value in a results array ([ts, value] pairs). */
static bool last_value(const cJSON *results, double *out) {
    if (!cJSON_IsArray(results)) return false;
    int n = cJSON_GetArraySize(results);
    if (n == 0) return false;
    const cJSON *pair = cJSON_GetArrayItem(results, n - 1);
    if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) < 2) return false;
    const cJSON *v = cJSON_GetArrayItem(pair, 1);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

bool nem_oe_parse_power(const char *json, nem_region_mix_t *out) {
    memset(out, 0, sizeof(*out));
    if (!json) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) { cJSON_Delete(root); return false; }

    /* Accumulate renewable MW separately to compute the fraction. */
    double renewable_mw[NEM_REGION_COUNT] = {0};
    int series = 0;

    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, data) {
        const cJSON *reg = cJSON_GetObjectItemCaseSensitive(el, "network_region");
        const cJSON *ft  = cJSON_GetObjectItemCaseSensitive(el, "fueltech");
        if (!cJSON_IsString(reg) || !cJSON_IsString(ft)) continue;
        nem_region_t r = nem_region_from_short(reg->valuestring);
        if (r >= NEM_REGION_COUNT) continue;

        nem_fuel_t bucket; bool renewable, is_load;
        if (!nem_fueltech_map(ft->valuestring, &bucket, &renewable, &is_load)) continue;
        if (is_load) continue;

        double v = 0.0;
        if (!last_value(cJSON_GetObjectItemCaseSensitive(el, "results"), &v)) continue;

        nem_fuel_mix_t *m = &out->regions[r];
        m->mw[bucket] += v;
        m->total_mw += v;
        if (renewable) renewable_mw[r] += v;
        m->valid = true;
        series++;
    }

    for (int r = 0; r < NEM_REGION_COUNT; r++) {
        nem_fuel_mix_t *m = &out->regions[r];
        m->renewable_fraction = (m->total_mw > 0.0) ? renewable_mw[r] / m->total_mw : 0.0;
    }

    cJSON_Delete(root);
    return series > 0;
}
