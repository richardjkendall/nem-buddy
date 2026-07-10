#include "unity.h"
#include "nem/alerts.h"
#include "nem/config.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static nem_snapshot_t snap_with(nem_region_t r, double price, double demand) {
    nem_snapshot_t s; memset(&s, 0, sizeof(s));
    for (int i = 0; i < NEM_REGION_COUNT; i++) s.regions[i].region = (nem_region_t)i;
    s.regions[r].valid = true;
    s.regions[r].price = price;
    s.regions[r].demand_mw = demand;
    return s;
}

static void test_spike_fires_once_then_debounces(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];

    nem_snapshot_t s = snap_with(NEM_REGION_VIC, 450.0, 6000.0); /* > 300, < 1000 */
    int n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_SPIKE, ev[0].type);
    TEST_ASSERT_EQUAL_INT(NEM_REGION_VIC, ev[0].region);

    /* Still spiking -> no new event. */
    n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(0, n);

    /* Clears, then spikes again -> fires again. */
    nem_snapshot_t calm = snap_with(NEM_REGION_VIC, 90.0, 6000.0);
    nem_alerts_evaluate(&st, &cfg, &calm, NULL, ev, 8);
    n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
}

static void test_extreme_supersedes_spike(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t s = snap_with(NEM_REGION_VIC, 9420.0, 6000.0);
    int n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_EXTREME_SPIKE, ev[0].type);
}

static void test_negative_price(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t s = snap_with(NEM_REGION_SA, -18.0, 1200.0);
    int n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_NEGATIVE, ev[0].type);
}

static void test_high_demand_uses_configured_mark(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    cfg.thresholds.high_demand_mw[NEM_REGION_NSW] = 12000.0;
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t under = snap_with(NEM_REGION_NSW, 100.0, 11000.0);
    TEST_ASSERT_EQUAL_INT(0, nem_alerts_evaluate(&st, &cfg, &under, NULL, ev, 8));
    nem_snapshot_t over = snap_with(NEM_REGION_NSW, 100.0, 12500.0);
    int n = nem_alerts_evaluate(&st, &cfg, &over, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_HIGH_DEMAND, ev[0].type);
}

static void test_high_renewable_needs_mix(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t s = snap_with(NEM_REGION_SA, 40.0, 1200.0);

    nem_region_mix_t mix; memset(&mix, 0, sizeof(mix));
    mix.regions[NEM_REGION_SA].valid = true;
    mix.regions[NEM_REGION_SA].renewable_fraction = 0.92;

    int n = nem_alerts_evaluate(&st, &cfg, &s, &mix, ev, 8);
    /* SA renewable event present among results */
    bool found = false;
    for (int i = 0; i < n; i++)
        if (ev[i].type == NEM_ALERT_HIGH_RENEWABLE && ev[i].region == NEM_REGION_SA) found = true;
    TEST_ASSERT_TRUE(found);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_spike_fires_once_then_debounces);
    RUN_TEST(test_extreme_supersedes_spike);
    RUN_TEST(test_negative_price);
    RUN_TEST(test_high_demand_uses_configured_mark);
    RUN_TEST(test_high_renewable_needs_mix);
    return UNITY_END();
}
