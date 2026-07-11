#ifndef NET_FETCH_H
#define NET_FETCH_H
#include <stddef.h>
#include "esp_err.h"
/* HTTPS GET into caller buffer (NUL-terminated). bearer may be NULL.
 * Returns ESP_OK only on HTTP 200. */
esp_err_t nem_http_get(const char *url, const char *bearer, char *buf, size_t buf_sz, int *out_len);
#endif
