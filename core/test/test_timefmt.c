/* core/test/test_timefmt.c */
#include "unity.h"
#include <string.h>
#include "nem/timefmt.h"

void setUp(void) {}
void tearDown(void) {}

static void expect(long long s, const char *want) {
    char b[24];
    nem_fmt_ago(s, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING(want, b);
}

static void test_never_for_negative(void) {
    expect(-1, "never");
    expect(-999, "never");
}

static void test_seconds(void) {
    expect(0,  "0s ago");
    expect(12, "12s ago");
    expect(59, "59s ago");
}

static void test_minutes(void) {
    expect(60,   "1m ago");
    expect(179,  "2m ago");
    expect(3599, "59m ago");
}

static void test_hours(void) {
    expect(3600,  "1h ago");
    expect(86399, "23h ago");
}

static void test_days(void) {
    expect(86400,  "1d ago");
    expect(259200, "3d ago");
}

static void test_truncates_rather_than_overflows(void) {
    char b[4];
    nem_fmt_ago(123456, b, sizeof b);
    TEST_ASSERT_TRUE(strlen(b) < sizeof b);   /* NUL-terminated, no overrun */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_never_for_negative);
    RUN_TEST(test_seconds);
    RUN_TEST(test_minutes);
    RUN_TEST(test_hours);
    RUN_TEST(test_days);
    RUN_TEST(test_truncates_rather_than_overflows);
    return UNITY_END();
}
