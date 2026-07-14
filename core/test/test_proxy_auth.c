#include "unity.h"
#include "nem/proxy_auth.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_req_msg_constant(void) {
    TEST_ASSERT_EQUAL_STRING("GET /nem", NEM_AUTH_REQ_MSG);
}

static void test_accept_fresh_rules(void) {
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 0));    /* first ever */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(200, 100));  /* newer  */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 100));  /* same == accept */
    TEST_ASSERT_FALSE(nem_auth_accept_fresh(99, 100));  /* strictly older = replay */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_req_msg_constant);
    RUN_TEST(test_accept_fresh_rules);
    return UNITY_END();
}
