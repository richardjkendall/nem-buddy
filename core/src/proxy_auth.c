#include "nem/proxy_auth.h"

bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch) {
    return new_epoch >= last_epoch;
}
