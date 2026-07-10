#ifndef NEM_FUEL_H
#define NEM_FUEL_H

#include <stdbool.h>
#include "nem/regions.h"

typedef enum {
    NEM_FUEL_COAL = 0,
    NEM_FUEL_GAS,
    NEM_FUEL_HYDRO,
    NEM_FUEL_WIND,
    NEM_FUEL_SOLAR,
    NEM_FUEL_BATTERY,
    NEM_FUEL_OTHER,
    NEM_FUEL_COUNT
} nem_fuel_t;

typedef struct {
    double mw[NEM_FUEL_COUNT];
    double total_mw;
    double renewable_fraction; /* 0..1 */
    bool   valid;
} nem_fuel_mix_t;

typedef struct {
    nem_fuel_mix_t regions[NEM_REGION_COUNT];
} nem_region_mix_t;

bool nem_fueltech_map(const char *ft, nem_fuel_t *bucket, bool *renewable, bool *is_load);

#endif
