#include "unity.h"
#include "nem/proxy_auth.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_req_message_format(void) {
    char buf[32];
    int n = nem_auth_req_message(buf, sizeof buf, 42ULL);
    TEST_ASSERT_EQUAL_STRING("GET /nem\n42", buf);
    TEST_ASSERT_EQUAL_INT(11, n);   /* strlen("GET /nem\n42") */
}

static void test_req_message_large_counter(void) {
    char buf[40];
    nem_auth_req_message(buf, sizeof buf, 4294968320ULL);   /* > 32-bit */
    TEST_ASSERT_EQUAL_STRING("GET /nem\n4294968320", buf);
}

static void test_accept_fresh_rules(void) {
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 0));    /* first ever */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(200, 100));  /* newer  */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 100));  /* same == no-op, accept */
    TEST_ASSERT_FALSE(nem_auth_accept_fresh(99, 100));  /* strictly older = replay */
}

static void test_reserve(void) {
    unsigned long long new_floor = 0;
    unsigned long long start = nem_auth_reserve(1000ULL, 1024ULL, &new_floor);
    TEST_ASSERT_EQUAL_UINT64(1000ULL, start);
    TEST_ASSERT_EQUAL_UINT64(2024ULL, new_floor);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_req_message_format);
    RUN_TEST(test_req_message_large_counter);
    RUN_TEST(test_accept_fresh_rules);
    RUN_TEST(test_reserve);
    return UNITY_END();
}
