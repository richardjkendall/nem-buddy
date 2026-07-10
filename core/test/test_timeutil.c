#include "unity.h"
#include "nem/timeutil.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse_known(void) {
    long long e = 0;
    /* 2026-07-10T19:25:00 UTC = 1783711500 */
    TEST_ASSERT_TRUE(nem_parse_iso8601("2026-07-10T19:25:00", &e));
    TEST_ASSERT_EQUAL_INT64(1783711500LL, e);
}

static void test_parse_epoch_zero(void) {
    long long e = -1;
    TEST_ASSERT_TRUE(nem_parse_iso8601("1970-01-01T00:00:00", &e));
    TEST_ASSERT_EQUAL_INT64(0LL, e);
}

static void test_parse_rejects_malformed(void) {
    long long e = 0;
    TEST_ASSERT_FALSE(nem_parse_iso8601("2026-07-10 19:25:00", &e)); /* space, not T */
    TEST_ASSERT_FALSE(nem_parse_iso8601("not-a-date", &e));
    TEST_ASSERT_FALSE(nem_parse_iso8601(NULL, &e));
}

static void test_minute_and_day(void) {
    long long e;
    nem_parse_iso8601("2026-07-10T19:25:00", &e);
    TEST_ASSERT_EQUAL_INT(19 * 60 + 25, nem_minute_of_day(e));
    long long a, b;
    nem_parse_iso8601("2026-07-10T23:59:00", &a);
    nem_parse_iso8601("2026-07-11T00:01:00", &b);
    TEST_ASSERT_EQUAL_INT(1, nem_day_index(b) - nem_day_index(a));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_known);
    RUN_TEST(test_parse_epoch_zero);
    RUN_TEST(test_parse_rejects_malformed);
    RUN_TEST(test_minute_and_day);
    return UNITY_END();
}
