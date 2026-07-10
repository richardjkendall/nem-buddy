#include "nem/config.h"

void nem_config_defaults(nem_config_t *cfg) {
    cfg->home_region = NEM_REGION_VIC;
    cfg->thresholds.spike_price = 300.0;
    cfg->thresholds.extreme_spike_price = 1000.0;
    cfg->thresholds.negative_price = 0.0;
    cfg->thresholds.high_renewable_frac = 0.80;
    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        cfg->thresholds.high_demand_mw[i] = 0.0; /* disabled until set */
    }
    cfg->quiet_start_hour = 22;
    cfg->quiet_end_hour = 7;
    cfg->chime_muted = false;
    cfg->brightness = 80;
    cfg->alert_auto_dismiss_s = 30;
}

bool nem_config_validate(const nem_config_t *cfg) {
    if (cfg->home_region < 0 || cfg->home_region >= NEM_REGION_COUNT) return false;
    if (cfg->brightness > 100) return false;
    if (cfg->quiet_start_hour > 23 || cfg->quiet_end_hour > 23) return false;
    if (cfg->thresholds.extreme_spike_price < cfg->thresholds.spike_price) return false;
    if (cfg->thresholds.high_renewable_frac < 0.0 || cfg->thresholds.high_renewable_frac > 1.0) return false;
    if (cfg->alert_auto_dismiss_s == 0) return false;
    return true;
}

bool nem_config_in_quiet_hours(const nem_config_t *cfg, int hour) {
    int s = cfg->quiet_start_hour, e = cfg->quiet_end_hour;
    if (s == e) return false;             /* no quiet window */
    if (s < e) return hour >= s && hour < e;
    return hour >= s || hour < e;          /* wraps midnight  */
}

bool nem_config_should_chime(const nem_config_t *cfg, int hour) {
    return !cfg->chime_muted && !nem_config_in_quiet_hours(cfg, hour);
}
