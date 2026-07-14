#ifndef NEM_PROXY_AUTH_H
#define NEM_PROXY_AUTH_H

#include <stdbool.h>

/* Canonical request-MAC message (constant). The request MAC proves possession of
 * the device key without transmitting it; no counter, no clock. */
#define NEM_AUTH_REQ_MSG "GET /nem"

/* Freshness: accept a payload iff its settlement epoch is not strictly older. */
bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch);

#endif
