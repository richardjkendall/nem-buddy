#ifndef NEM_PROXY_AUTH_H
#define NEM_PROXY_AUTH_H

#include <stdbool.h>
#include <stddef.h>

/* Canonical request-MAC message: "GET /nem\n<counter>". Returns snprintf length. */
int nem_auth_req_message(char *out, size_t out_sz, unsigned long long counter);

/* Freshness: accept a payload iff its settlement epoch is not strictly older. */
bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch);

/* Counter reservation: RAM counter starts at stored_floor; the new NVS floor to
 * persist is stored_floor + gap (guarantees monotonic counters across reboots). */
unsigned long long nem_auth_reserve(unsigned long long stored_floor,
                                    unsigned long long gap,
                                    unsigned long long *out_new_floor);

#endif
