#include "unity.h"
#include "nem/regions.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_id_and_name_roundtrip(void) {
    TEST_ASSERT_EQUAL_STRING("VIC1", nem_region_id(NEM_REGION_VIC));
    TEST_ASSERT_EQUAL_STRING("VIC",  nem_region_name(NEM_REGION_VIC));
    TEST_ASSERT_EQUAL_STRING("SA1",  nem_region_id(NEM_REGION_SA));
}

static void test_from_id(void) {
    TEST_ASSERT_EQUAL_INT(NEM_REGION_NSW, nem_region_from_id("NSW1"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_TAS, nem_region_from_id("TAS1"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_COUNT, nem_region_from_id("WA1"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_COUNT, nem_region_from_id(NULL));
}

static void test_from_short(void) {
    TEST_ASSERT_EQUAL_INT(NEM_REGION_QLD, nem_region_from_short("QLD"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_COUNT, nem_region_from_short("ACT"));
}

static void test_out_of_range(void) {
    TEST_ASSERT_EQUAL_STRING("?", nem_region_id(NEM_REGION_COUNT));
    TEST_ASSERT_EQUAL_STRING("?", nem_region_name(NEM_REGION_COUNT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_id_and_name_roundtrip);
    RUN_TEST(test_from_id);
    RUN_TEST(test_from_short);
    RUN_TEST(test_out_of_range);
    return UNITY_END();
}
