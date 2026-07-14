#ifndef NET_FETCH_H
#define NET_FETCH_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* key = 32-byte device key, or key=NULL for LAN mode (no auth, no verify).
 * device_id is sent as X-NEM-Id; NULL/empty means LAN mode. */
typedef struct {
    const uint8_t *key;
    const char    *device_id;
} nem_auth_t;

/* HTTP GET into caller buffer (NUL-terminated). When auth->key is set: signs the
 * request (X-NEM-Id/X-NEM-Auth) and verifies the response signature (X-NEM-Sig),
 * failing closed. Returns ESP_OK only on HTTP 200 AND (LAN mode OR valid sig). */
esp_err_t nem_http_get(const char *url, const nem_auth_t *auth, char *buf, size_t buf_sz, int *out_len);

/* One-shot known-answer self-test of the on-device HMAC/base64 path. Logs + returns result. */
bool nem_http_auth_selftest(void);
#endif
