#include "unity.h"
#include "nem/proxy_client.h"
#include "nem/timeutil.h"
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

static void test_parses_price_demand_and_mix(void) {
    char *json = read_fixture("proxy_sample.json");
    nem_snapshot_t s; nem_region_mix_t m;
    TEST_ASSERT_TRUE(nem_proxy_parse(json, &s, &m));

    const nem_region_snapshot_t *vic = &s.regions[NEM_REGION_VIC];
    TEST_ASSERT_TRUE(vic->valid);
    TEST_ASSERT_EQUAL_DOUBLE(19.24, vic->price);
    TEST_ASSERT_EQUAL_DOUBLE(7477.0, vic->demand_mw);

    const nem_fuel_mix_t *vm = &m.regions[NEM_REGION_VIC];
    TEST_ASSERT_TRUE(vm->valid);
    TEST_ASSERT_EQUAL_DOUBLE(4551.0, vm->mw[NEM_FUEL_COAL]);
    TEST_ASSERT_EQUAL_DOUBLE(2978.0, vm->mw[NEM_FUEL_WIND]);
    TEST_ASSERT_EQUAL_DOUBLE(627.0,  vm->mw[NEM_FUEL_BATTERY]);
    TEST_ASSERT_EQUAL_DOUBLE(40.0,   vm->mw[NEM_FUEL_HYDRO]);
    TEST_ASSERT_EQUAL_DOUBLE(4551.0 + 2978.0 + 627.0 + 40.0, vm->total_mw);
    TEST_ASSERT_TRUE(fabs(vm->renewable_fraction - 0.445) < 1e-9);

    const nem_region_snapshot_t *sa = &s.regions[NEM_REGION_SA];
    TEST_ASSERT_TRUE(sa->valid);
    TEST_ASSERT_EQUAL_DOUBLE(-4.67, sa->price);

    /* regions absent from the payload stay invalid */
    TEST_ASSERT_FALSE(s.regions[NEM_REGION_NSW].valid);
    TEST_ASSERT_FALSE(m.regions[NEM_REGION_NSW].valid);
    free(json);
}

static void test_parses_interconnectors_and_epoch(void) {
    char *json = read_fixture("proxy_sample.json");
    nem_snapshot_t s; nem_region_mix_t m;
    TEST_ASSERT_TRUE(nem_proxy_parse(json, &s, &m));

    long long expect_epoch = 0;
    TEST_ASSERT_TRUE(nem_parse_iso8601("2026-07-11T17:55:00", &expect_epoch));

    const nem_region_snapshot_t *vic = &s.regions[NEM_REGION_VIC];
    TEST_ASSERT_EQUAL_INT64(expect_epoch, vic->settlement_epoch);
    TEST_ASSERT_EQUAL_DOUBLE(1064.7, vic->net_interchange);
    TEST_ASSERT_EQUAL_INT(3, vic->interconnector_count);
    TEST_ASSERT_EQUAL_STRING("T-V-MNSP1", vic->interconnectors[0].name);
    TEST_ASSERT_EQUAL_DOUBLE(311.3, vic->interconnectors[0].value);
    TEST_ASSERT_EQUAL_STRING("VIC1-NSW1", vic->interconnectors[1].name);
    TEST_ASSERT_EQUAL_DOUBLE(528.8, vic->interconnectors[1].value);

    const nem_region_snapshot_t *sa = &s.regions[NEM_REGION_SA];
    TEST_ASSERT_EQUAL_INT(1, sa->interconnector_count);
    TEST_ASSERT_EQUAL_DOUBLE(-224.6, sa->interconnectors[0].value);
    TEST_ASSERT_EQUAL_INT64(expect_epoch, sa->settlement_epoch);

    free(json);
}

static void test_rejects_garbage(void) {
    nem_snapshot_t s; nem_region_mix_t m;
    TEST_ASSERT_FALSE(nem_proxy_parse("{}", &s, &m));
    TEST_ASSERT_FALSE(nem_proxy_parse("not json", &s, &m));
    TEST_ASSERT_FALSE(nem_proxy_parse(NULL, &s, &m));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_price_demand_and_mix);
    RUN_TEST(test_parses_interconnectors_and_epoch);
    RUN_TEST(test_rejects_garbage);
    return UNITY_END();
}
