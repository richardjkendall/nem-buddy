#include "unity.h"
#include "nem/history.h"
#include "nem/timeutil.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static nem_snapshot_t make_snap(double vic_price, double vic_demand) {
    nem_snapshot_t s; memset(&s, 0, sizeof(s));
    for (int i = 0; i < NEM_REGION_COUNT; i++) s.regions[i].region = (nem_region_t)i;
    s.regions[NEM_REGION_VIC].valid = true;
    s.regions[NEM_REGION_VIC].price = vic_price;
    s.regions[NEM_REGION_VIC].demand_mw = vic_demand;
    return s;
}

static void test_add_and_bucket(void) {
    nem_history_t h; nem_history_init(&h);
    long long e; nem_parse_iso8601("2026-07-10T19:25:00", &e); /* minute 1165 -> slot 233 */
    nem_snapshot_t s = make_snap(96.7, 6240.0);
    nem_history_add(&h, &s, e);

    const nem_region_history_t *vic = &h.regions[NEM_REGION_VIC];
    int slot = (19 * 60 + 25) / 5;
    TEST_ASSERT_TRUE(vic->filled[slot]);
    TEST_ASSERT_EQUAL_DOUBLE(96.7, vic->price[slot]);
    TEST_ASSERT_EQUAL_INT(1, nem_history_filled_count(vic));
}

static void test_same_slot_overwrites(void) {
    nem_history_t h; nem_history_init(&h);
    long long a, b;
    nem_parse_iso8601("2026-07-10T19:25:00", &a);
    nem_parse_iso8601("2026-07-10T19:27:00", &b); /* same 5-min slot */
    nem_history_add(&h, &(nem_snapshot_t){0}, a); /* invalid region -> skipped */
    nem_snapshot_t s1 = make_snap(50.0, 100.0);
    nem_snapshot_t s2 = make_snap(60.0, 110.0);
    nem_history_add(&h, &s1, a);
    nem_history_add(&h, &s2, b);
    int slot = (19 * 60 + 25) / 5;
    TEST_ASSERT_EQUAL_DOUBLE(60.0, h.regions[NEM_REGION_VIC].price[slot]);
    TEST_ASSERT_EQUAL_INT(1, nem_history_filled_count(&h.regions[NEM_REGION_VIC]));
}

static void test_new_day_resets(void) {
    nem_history_t h; nem_history_init(&h);
    long long d1, d2;
    nem_parse_iso8601("2026-07-10T23:55:00", &d1);
    nem_parse_iso8601("2026-07-11T00:05:00", &d2);
    nem_snapshot_t s = make_snap(80.0, 5000.0);
    nem_history_add(&h, &s, d1);
    nem_history_add(&h, &s, d2);
    /* After rollover only the new day's single sample remains. */
    TEST_ASSERT_EQUAL_INT(1, nem_history_filled_count(&h.regions[NEM_REGION_VIC]));
    int slot_late = (23 * 60 + 55) / 5;
    TEST_ASSERT_FALSE(h.regions[NEM_REGION_VIC].filled[slot_late]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_and_bucket);
    RUN_TEST(test_same_slot_overwrites);
    RUN_TEST(test_new_day_resets);
    return UNITY_END();
}
