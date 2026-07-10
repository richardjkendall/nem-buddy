#ifndef NEM_CONFIG_H
#define NEM_CONFIG_H

#include <stdbool.h>
#include "nem/regions.h"

typedef struct {
    double spike_price;
    double extreme_spike_price;
    double negative_price;
    double high_demand_mw[NEM_REGION_COUNT];
    double high_renewable_frac;
} nem_thresholds_t;

typedef struct {
    nem_region_t   home_region;
    nem_thresholds_t thresholds;
    unsigned char  quiet_start_hour;   /* 0..23 */
    unsigned char  quiet_end_hour;     /* 0..23 */
    bool           chime_muted;
    unsigned char  brightness;         /* 0..100 */
    unsigned short alert_auto_dismiss_s;
} nem_config_t;

void nem_config_defaults(nem_config_t *cfg);
bool nem_config_validate(const nem_config_t *cfg);
bool nem_config_in_quiet_hours(const nem_config_t *cfg, int hour);
bool nem_config_should_chime(const nem_config_t *cfg, int hour);

#endif
