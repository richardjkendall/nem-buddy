#ifndef NEM_ALERTS_H
#define NEM_ALERTS_H

#include <stdbool.h>
#include "nem/snapshot.h"
#include "nem/config.h"
#include "nem/fuel.h"

typedef enum {
    NEM_ALERT_NONE = 0,
    NEM_ALERT_SPIKE,
    NEM_ALERT_EXTREME_SPIKE,
    NEM_ALERT_NEGATIVE,
    NEM_ALERT_HIGH_DEMAND,
    NEM_ALERT_HIGH_RENEWABLE,
    NEM_ALERT_TYPE_COUNT
} nem_alert_type_t;

typedef struct {
    nem_alert_type_t type;
    nem_region_t     region;
    double           value;
} nem_alert_event_t;

typedef struct {
    bool active[NEM_REGION_COUNT][NEM_ALERT_TYPE_COUNT];
} nem_alert_state_t;

void nem_alerts_init(nem_alert_state_t *st);
int  nem_alerts_evaluate(nem_alert_state_t *st, const nem_config_t *cfg,
                         const nem_snapshot_t *snap, const nem_region_mix_t *mix,
                         nem_alert_event_t *events, int max_events);

#endif
