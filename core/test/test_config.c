#include "unity.h"
#include "nem/config.h"

void setUp(void) {}
void tearDown(void) {}

static void test_defaults(void) {
    nem_config_t c;
    nem_config_defaults(&c);
    TEST_ASSERT_EQUAL_INT(NEM_REGION_VIC, c.home_region);
    TEST_ASSERT_EQUAL_DOUBLE(300.0,  c.thresholds.spike_price);
    TEST_ASSERT_EQUAL_DOUBLE(1000.0, c.thresholds.extreme_spike_price);
    TEST_ASSERT_EQUAL_DOUBLE(0.0,    c.thresholds.negative_price);
    TEST_ASSERT_EQUAL_DOUBLE(0.80,   c.thresholds.high_renewable_frac);
    TEST_ASSERT_EQUAL_UINT8(30, c.alert_auto_dismiss_s == 30 ? 30 : 0);
    TEST_ASSERT_TRUE(nem_config_validate(&c));
}

static void test_validate_rejects_bad(void) {
    nem_config_t c; nem_config_defaults(&c);
    c.brightness = 200;                 /* > 100 */
    TEST_ASSERT_FALSE(nem_config_validate(&c));
    nem_config_defaults(&c);
    c.thresholds.extreme_spike_price = 100.0; /* below spike */
    TEST_ASSERT_FALSE(nem_config_validate(&c));
}

static void test_quiet_hours_wrap(void) {
    nem_config_t c; nem_config_defaults(&c); /* quiet 22 -> 7 */
    TEST_ASSERT_TRUE(nem_config_in_quiet_hours(&c, 23));
    TEST_ASSERT_TRUE(nem_config_in_quiet_hours(&c, 3));
    TEST_ASSERT_TRUE(nem_config_in_quiet_hours(&c, 22));  /* inclusive start */
    TEST_ASSERT_FALSE(nem_config_in_quiet_hours(&c, 7));  /* exclusive end   */
    TEST_ASSERT_FALSE(nem_config_in_quiet_hours(&c, 12));
}

static void test_should_chime(void) {
    nem_config_t c; nem_config_defaults(&c);
    TEST_ASSERT_TRUE(nem_config_should_chime(&c, 12));
    TEST_ASSERT_FALSE(nem_config_should_chime(&c, 2));  /* quiet hours */
    c.chime_muted = true;
    TEST_ASSERT_FALSE(nem_config_should_chime(&c, 12)); /* muted        */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_validate_rejects_bad);
    RUN_TEST(test_quiet_hours_wrap);
    RUN_TEST(test_should_chime);
    return UNITY_END();
}
