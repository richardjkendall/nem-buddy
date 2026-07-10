#include "nem/regions.h"
#include <string.h>

static const char *const IDS[NEM_REGION_COUNT]    = { "NSW1", "QLD1", "SA1", "TAS1", "VIC1" };
static const char *const NAMES[NEM_REGION_COUNT]   = { "NSW",  "QLD",  "SA",  "TAS",  "VIC"  };

const char *nem_region_id(nem_region_t r) {
    return (r >= 0 && r < NEM_REGION_COUNT) ? IDS[r] : "?";
}

const char *nem_region_name(nem_region_t r) {
    return (r >= 0 && r < NEM_REGION_COUNT) ? NAMES[r] : "?";
}

nem_region_t nem_region_from_id(const char *region_id) {
    if (!region_id) return NEM_REGION_COUNT;
    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        if (strcmp(region_id, IDS[i]) == 0) return (nem_region_t)i;
    }
    return NEM_REGION_COUNT;
}

nem_region_t nem_region_from_short(const char *short_name) {
    if (!short_name) return NEM_REGION_COUNT;
    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        if (strcmp(short_name, NAMES[i]) == 0) return (nem_region_t)i;
    }
    return NEM_REGION_COUNT;
}
