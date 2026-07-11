#include "net_fetch.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "fetch";

esp_err_t nem_http_get(const char *url, const char *bearer, char *buf, size_t buf_sz, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "nem-buddy/0.1",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    if (bearer) {
        char hdr[600];
        snprintf(hdr, sizeof(hdr), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return err; }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    int total = 0, r;
    while ((r = esp_http_client_read(c, buf + total, (int)buf_sz - 1 - total)) > 0) {
        total += r;
        if (total >= (int)buf_sz - 1) break;
    }
    buf[total < (int)buf_sz ? total : (int)buf_sz - 1] = 0;
    *out_len = total;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "GET %s -> %d, %d bytes", url, status, total);
    return status == 200 ? ESP_OK : ESP_FAIL;
}
