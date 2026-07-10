#include "nem/alerts.h"

void nem_alerts_init(nem_alert_state_t *st) {
    for (int r = 0; r < NEM_REGION_COUNT; r++)
        for (int t = 0; t < NEM_ALERT_TYPE_COUNT; t++)
            st->active[r][t] = false;
}

/* Emit on rising edge only: fire when condition true and not already active. */
static int edge(nem_alert_state_t *st, int r, nem_alert_type_t t, bool cond,
                double value, nem_alert_event_t *events, int max, int n) {
    if (cond) {
        if (!st->active[r][t]) {
            st->active[r][t] = true;
            if (n < max) {
                events[n].type = t;
                events[n].region = (nem_region_t)r;
                events[n].value = value;
                return n + 1;
            }
        }
    } else {
        st->active[r][t] = false;
    }
    return n;
}

int nem_alerts_evaluate(nem_alert_state_t *st, const nem_config_t *cfg,
                        const nem_snapshot_t *snap, const nem_region_mix_t *mix,
                        nem_alert_event_t *events, int max_events) {
    const nem_thresholds_t *th = &cfg->thresholds;
    int n = 0;

    for (int r = 0; r < NEM_REGION_COUNT; r++) {
        const nem_region_snapshot_t *rs = &snap->regions[r];
        if (!rs->valid) continue;

        bool extreme = rs->price > th->extreme_spike_price;
        bool spike   = rs->price > th->spike_price && !extreme; /* extreme supersedes */
        n = edge(st, r, NEM_ALERT_EXTREME_SPIKE, extreme, rs->price, events, max_events, n);
        n = edge(st, r, NEM_ALERT_SPIKE,         spike,   rs->price, events, max_events, n);

        bool negative = rs->price < th->negative_price;
        n = edge(st, r, NEM_ALERT_NEGATIVE, negative, rs->price, events, max_events, n);

        bool high_demand = th->high_demand_mw[r] > 0.0 && rs->demand_mw > th->high_demand_mw[r];
        n = edge(st, r, NEM_ALERT_HIGH_DEMAND, high_demand, rs->demand_mw, events, max_events, n);

        bool high_ren = false; double ren_val = 0.0;
        if (mix && mix->regions[r].valid) {
            ren_val = mix->regions[r].renewable_fraction;
            high_ren = ren_val > th->high_renewable_frac;
        }
        n = edge(st, r, NEM_ALERT_HIGH_RENEWABLE, high_ren, ren_val, events, max_events, n);
    }
    return n;
}
