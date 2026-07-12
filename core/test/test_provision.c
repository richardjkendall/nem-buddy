#include "unity.h"
#include "nem/provision.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_parse_happy(void) {
    const char *b = "ssid=MyNet&password=s3cret&proxy_url=http%3A%2F%2F1.2.3.4%3A8080%2Fnem&proxy_token=abc";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("MyNet", f.ssid);
    TEST_ASSERT_EQUAL_STRING("s3cret", f.password);
    TEST_ASSERT_EQUAL_STRING("http://1.2.3.4:8080/nem", f.proxy_url);
    TEST_ASSERT_EQUAL_STRING("abc", f.proxy_token);
}

static void test_parse_plus_is_space(void) {
    const char *b = "ssid=my+home+net&password=";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("my home net", f.ssid);
    TEST_ASSERT_EQUAL_STRING("", f.password);
}

static void test_parse_missing_ssid_fails(void) {
    const char *b = "password=x&proxy_url=y";
    nem_prov_form_t f;
    TEST_ASSERT_FALSE(nem_provision_parse_form(b, strlen(b), &f));
}

static void test_parse_empty_ssid_fails(void) {
    const char *b = "ssid=&password=x";
    nem_prov_form_t f;
    TEST_ASSERT_FALSE(nem_provision_parse_form(b, strlen(b), &f));
}

static void test_parse_overlong_ssid_fails(void) {
    /* 40 chars > NEM_PROV_SSID_MAX (32) */
    const char *b = "ssid=0123456789012345678901234567890123456789";
    nem_prov_form_t f;
    TEST_ASSERT_FALSE(nem_provision_parse_form(b, strlen(b), &f));
}

static void test_parse_ignores_unknown_and_last_wins(void) {
    const char *b = "junk=1&ssid=__other__&extra=2&ssid=Real";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("Real", f.ssid);   /* later key overwrites earlier */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_happy);
    RUN_TEST(test_parse_plus_is_space);
    RUN_TEST(test_parse_missing_ssid_fails);
    RUN_TEST(test_parse_empty_ssid_fails);
    RUN_TEST(test_parse_overlong_ssid_fails);
    RUN_TEST(test_parse_ignores_unknown_and_last_wins);
    return UNITY_END();
}
