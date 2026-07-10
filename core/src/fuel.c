#include "nem/fuel.h"
#include <string.h>

typedef struct { const char *ft; nem_fuel_t bucket; bool renewable; bool is_load; } map_row_t;

static const map_row_t ROWS[] = {
    { "coal_black",          NEM_FUEL_COAL,    false, false },
    { "coal_brown",          NEM_FUEL_COAL,    false, false },
    { "gas_ccgt",            NEM_FUEL_GAS,     false, false },
    { "gas_ocgt",            NEM_FUEL_GAS,     false, false },
    { "gas_recip",           NEM_FUEL_GAS,     false, false },
    { "gas_steam",           NEM_FUEL_GAS,     false, false },
    { "gas_wcmg",            NEM_FUEL_GAS,     false, false },
    { "distillate",          NEM_FUEL_OTHER,   false, false },
    { "bioenergy_biomass",   NEM_FUEL_OTHER,   true,  false },
    { "bioenergy_biogas",    NEM_FUEL_OTHER,   true,  false },
    { "hydro",               NEM_FUEL_HYDRO,   true,  false },
    { "wind",                NEM_FUEL_WIND,    true,  false },
    { "solar_utility",       NEM_FUEL_SOLAR,   true,  false },
    { "solar_rooftop",       NEM_FUEL_SOLAR,   true,  false },
    { "battery_discharging", NEM_FUEL_BATTERY, true,  false },
    { "battery_charging",    NEM_FUEL_BATTERY, false, true  },
    { "pumps",               NEM_FUEL_HYDRO,   false, true  },
};

bool nem_fueltech_map(const char *ft, nem_fuel_t *bucket, bool *renewable, bool *is_load) {
    if (!ft) return false;
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        if (strcmp(ft, ROWS[i].ft) == 0) {
            *bucket = ROWS[i].bucket;
            *renewable = ROWS[i].renewable;
            *is_load = ROWS[i].is_load;
            return true;
        }
    }
    return false;
}
