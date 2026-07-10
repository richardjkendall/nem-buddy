#include "unity.h"
#include "nem/fuel.h"

void setUp(void) {}
void tearDown(void) {}

static void test_maps_known(void) {
    nem_fuel_t b; bool ren, load;
    TEST_ASSERT_TRUE(nem_fueltech_map("coal_black", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_COAL, b); TEST_ASSERT_FALSE(ren); TEST_ASSERT_FALSE(load);

    TEST_ASSERT_TRUE(nem_fueltech_map("solar_rooftop", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_SOLAR, b); TEST_ASSERT_TRUE(ren);

    TEST_ASSERT_TRUE(nem_fueltech_map("wind", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_WIND, b); TEST_ASSERT_TRUE(ren);

    TEST_ASSERT_TRUE(nem_fueltech_map("battery_discharging", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_BATTERY, b); TEST_ASSERT_FALSE(load);

    TEST_ASSERT_TRUE(nem_fueltech_map("battery_charging", &b, &ren, &load));
    TEST_ASSERT_TRUE(load);  /* load: excluded from generation totals */

    TEST_ASSERT_TRUE(nem_fueltech_map("gas_ccgt", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_GAS, b); TEST_ASSERT_FALSE(ren);
}

static void test_unknown(void) {
    nem_fuel_t b; bool ren, load;
    TEST_ASSERT_FALSE(nem_fueltech_map("unobtainium", &b, &ren, &load));
    TEST_ASSERT_FALSE(nem_fueltech_map(NULL, &b, &ren, &load));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_maps_known);
    RUN_TEST(test_unknown);
    return UNITY_END();
}
