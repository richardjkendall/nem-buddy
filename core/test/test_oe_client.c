#include "unity.h"
#include "nem/oe_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

static char *read_fixture(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb"); TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f); return b;
}

static void test_parses_vic_mix(void) {
    char *json = read_fixture("oe_power_fueltech.json");
    nem_region_mix_t m;
    TEST_ASSERT_TRUE(nem_oe_parse_power(json, &m));

    const nem_fuel_mix_t *vic = &m.regions[NEM_REGION_VIC];
    TEST_ASSERT_TRUE(vic->valid);
    /* last coal_brown value 1900, wind 1300, solar 300; charging is load -> excluded */
    TEST_ASSERT_EQUAL_DOUBLE(1900.0, vic->mw[NEM_FUEL_COAL]);
    TEST_ASSERT_EQUAL_DOUBLE(1300.0, vic->mw[NEM_FUEL_WIND]);
    TEST_ASSERT_EQUAL_DOUBLE(300.0,  vic->mw[NEM_FUEL_SOLAR]);
    TEST_ASSERT_EQUAL_DOUBLE(3500.0, vic->total_mw);
    /* renewable = (1300+300)/3500 */
    TEST_ASSERT_TRUE(fabs(vic->renewable_fraction - (1600.0/3500.0)) < 1e-9);

    const nem_fuel_mix_t *sa = &m.regions[NEM_REGION_SA];
    TEST_ASSERT_TRUE(sa->valid);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, sa->renewable_fraction); /* all wind */
    free(json);
}

static void test_rejects_garbage(void) {
    nem_region_mix_t m;
    TEST_ASSERT_FALSE(nem_oe_parse_power("{}", &m));
    TEST_ASSERT_FALSE(nem_oe_parse_power(NULL, &m));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_vic_mix);
    RUN_TEST(test_rejects_garbage);
    return UNITY_END();
}
