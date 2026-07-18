#include "unity.h"
#include "nem/battery.h"

void setUp(void) {}
void tearDown(void) {}

static void test_clamps_rails(void) {
    TEST_ASSERT_EQUAL_UINT8(0,   nem_batt_pct_from_mv(0));
    TEST_ASSERT_EQUAL_UINT8(0,   nem_batt_pct_from_mv(3000));
    TEST_ASSERT_EQUAL_UINT8(0,   nem_batt_pct_from_mv(3300));
    TEST_ASSERT_EQUAL_UINT8(100, nem_batt_pct_from_mv(4200));
    TEST_ASSERT_EQUAL_UINT8(100, nem_batt_pct_from_mv(5000));
}

static void test_known_points(void) {
    TEST_ASSERT_EQUAL_UINT8(25,  nem_batt_pct_from_mv(3700));
    TEST_ASSERT_EQUAL_UINT8(55,  nem_batt_pct_from_mv(3800));
    TEST_ASSERT_EQUAL_UINT8(100, nem_batt_pct_from_mv(4200));
}

static void test_interpolates_between_points(void) {
    /* midway between 3700 (25%) and 3750 (40%) -> ~32% */
    uint8_t p = nem_batt_pct_from_mv(3725);
    TEST_ASSERT_TRUE(p > 25 && p < 40);
}

static void test_monotonic(void) {
    uint8_t prev = 0;
    for (uint16_t mv = 3300; mv <= 4200; mv += 10) {
        uint8_t p = nem_batt_pct_from_mv(mv);
        TEST_ASSERT_TRUE_MESSAGE(p >= prev, "curve must never decrease");
        prev = p;
    }
    TEST_ASSERT_EQUAL_UINT8(100, prev);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clamps_rails);
    RUN_TEST(test_known_points);
    RUN_TEST(test_interpolates_between_points);
    RUN_TEST(test_monotonic);
    return UNITY_END();
}
