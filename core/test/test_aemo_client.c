#include "unity.h"
#include "nem/aemo_client.h"
#include "nem/timeutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static char *read_fixture(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f); buf[n] = 0; fclose(f);
    return buf;
}

static void test_parses_two_regions(void) {
    char *json = read_fixture("aemo_summary.json");
    nem_snapshot_t s;
    TEST_ASSERT_TRUE(nem_aemo_parse_summary(json, &s));

    const nem_region_snapshot_t *vic = &s.regions[NEM_REGION_VIC];
    TEST_ASSERT_TRUE(vic->valid);
    TEST_ASSERT_EQUAL_DOUBLE(96.76914, vic->price);
    TEST_ASSERT_EQUAL_DOUBLE(6240.12, vic->demand_mw);
    TEST_ASSERT_EQUAL_DOUBLE(-420.5, vic->net_interchange);

    long long expect; nem_parse_iso8601("2026-07-10T19:25:00", &expect);
    TEST_ASSERT_EQUAL_INT64(expect, vic->settlement_epoch);

    TEST_ASSERT_EQUAL_INT(2, vic->interconnector_count);
    TEST_ASSERT_EQUAL_STRING("VIC1-NSW1", vic->interconnectors[0].name);
    TEST_ASSERT_EQUAL_DOUBLE(-311.2, vic->interconnectors[0].value);

    const nem_region_snapshot_t *sa = &s.regions[NEM_REGION_SA];
    TEST_ASSERT_TRUE(sa->valid);
    TEST_ASSERT_EQUAL_DOUBLE(-18.4, sa->price);
    TEST_ASSERT_EQUAL_INT(1, sa->interconnector_count);

    /* Regions absent from the feed stay invalid. */
    TEST_ASSERT_FALSE(s.regions[NEM_REGION_QLD].valid);
    free(json);
}

static void test_rejects_garbage(void) {
    nem_snapshot_t s;
    TEST_ASSERT_FALSE(nem_aemo_parse_summary("{not json", &s));
    TEST_ASSERT_FALSE(nem_aemo_parse_summary("{\"ELEC_NEM_SUMMARY\":[]}", &s));
    TEST_ASSERT_FALSE(nem_aemo_parse_summary(NULL, &s));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_two_regions);
    RUN_TEST(test_rejects_garbage);
    return UNITY_END();
}
