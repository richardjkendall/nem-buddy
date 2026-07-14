#include "nem/proxy_auth.h"
#include <stdio.h>

int nem_auth_req_message(char *out, size_t out_sz, unsigned long long counter) {
    return snprintf(out, out_sz, "GET /nem\n%llu", counter);
}

bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch) {
    return new_epoch >= last_epoch;
}

unsigned long long nem_auth_reserve(unsigned long long stored_floor,
                                    unsigned long long gap,
                                    unsigned long long *out_new_floor) {
    *out_new_floor = stored_floor + gap;
    return stored_floor;
}
