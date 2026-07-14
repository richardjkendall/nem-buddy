#include "unity.h"
#include "nem/provision.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_parse_happy(void) {
    const char *b = "ssid=MyNet&password=s3cret&proxy_url=http%3A%2F%2F1.2.3.4%3A8080%2Fnem&proxy_token=abc&device_id=dev01";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("MyNet", f.ssid);
    TEST_ASSERT_EQUAL_STRING("s3cret", f.password);
    TEST_ASSERT_EQUAL_STRING("http://1.2.3.4:8080/nem", f.proxy_url);
    TEST_ASSERT_EQUAL_STRING("abc", f.proxy_token);
    TEST_ASSERT_EQUAL_STRING("dev01", f.device_id);
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

static void test_dns_reply_shape(void) {
    /* Query: header (ID=0x1234, RD set, QD=1) + question www.example.com A IN */
    unsigned char q[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        3, 'w','w','w', 7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0,
        0x00, 0x01, 0x00, 0x01
    };
    unsigned char ip[4] = { 192, 168, 4, 1 };
    unsigned char out[128];
    int n = nem_provision_build_dns_reply(q, (int)sizeof q, ip, out, (int)sizeof out);

    TEST_ASSERT_EQUAL_INT((int)sizeof q + 16, n);   /* query + 16-byte answer */
    TEST_ASSERT_TRUE(out[2] & 0x80);                /* QR (response) bit set  */
    TEST_ASSERT_EQUAL_UINT8(0x00, out[6]);          /* ANCOUNT hi             */
    TEST_ASSERT_EQUAL_UINT8(0x01, out[7]);          /* ANCOUNT lo = 1         */
    /* Answer begins right after the copied query. */
    unsigned char *a = out + sizeof q;
    TEST_ASSERT_EQUAL_UINT8(0xC0, a[0]);            /* name pointer           */
    TEST_ASSERT_EQUAL_UINT8(0x0C, a[1]);            /* -> offset 12           */
    TEST_ASSERT_EQUAL_UINT8(0x00, a[2]);            /* TYPE A hi              */
    TEST_ASSERT_EQUAL_UINT8(0x01, a[3]);            /* TYPE A lo              */
    /* Last 4 bytes are the IP. */
    TEST_ASSERT_EQUAL_UINT8(192, out[n - 4]);
    TEST_ASSERT_EQUAL_UINT8(168, out[n - 3]);
    TEST_ASSERT_EQUAL_UINT8(4,   out[n - 2]);
    TEST_ASSERT_EQUAL_UINT8(1,   out[n - 1]);
}

static void test_dns_reply_rejects_short_and_small_buf(void) {
    unsigned char ip[4] = { 192, 168, 4, 1 };
    unsigned char out[128];
    unsigned char tiny[8] = {0};
    TEST_ASSERT_EQUAL_INT(-1, nem_provision_build_dns_reply(tiny, 8, ip, out, sizeof out));

    unsigned char q[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        3, 'w','w','w', 0, 0x00, 0x01, 0x00, 0x01
    };
    TEST_ASSERT_EQUAL_INT(-1, nem_provision_build_dns_reply(q, (int)sizeof q, ip, out, 10));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_happy);
    RUN_TEST(test_parse_plus_is_space);
    RUN_TEST(test_parse_missing_ssid_fails);
    RUN_TEST(test_parse_empty_ssid_fails);
    RUN_TEST(test_parse_overlong_ssid_fails);
    RUN_TEST(test_parse_ignores_unknown_and_last_wins);
    RUN_TEST(test_dns_reply_shape);
    RUN_TEST(test_dns_reply_rejects_short_and_small_buf);
    return UNITY_END();
}
