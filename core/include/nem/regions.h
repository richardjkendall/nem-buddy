#ifndef NEM_REGIONS_H
#define NEM_REGIONS_H

typedef enum {
    NEM_REGION_NSW = 0,
    NEM_REGION_QLD,
    NEM_REGION_SA,
    NEM_REGION_TAS,
    NEM_REGION_VIC,
    NEM_REGION_COUNT
} nem_region_t;

const char  *nem_region_id(nem_region_t r);
const char  *nem_region_name(nem_region_t r);
nem_region_t nem_region_from_id(const char *region_id);
nem_region_t nem_region_from_short(const char *short_name);

#endif
