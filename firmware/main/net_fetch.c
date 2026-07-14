#include "net_fetch.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "nem/proxy_auth.h"

static const char *TAG = "fetch";

/* HMAC-SHA256(key[32], msg) -> base64 (44 chars + NUL) in out (>=45 bytes). */
static void hmac_b64(const uint8_t key[32], const uint8_t *msg, size_t len, char out[45]) {
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, msg, len, mac);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, 45, &olen, mac, 32);
    out[olen] = 0;
}

/* constant-time equality of two NUL-terminated strings of equal expected length */
static bool ct_eq(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na != nb) return false;
    int d = 0;
    for (size_t i = 0; i < na; i++) d |= (a[i] ^ b[i]);
    return d == 0;
}

/* Response-signature capture. esp_http_client_get_header() only returns REQUEST
 * headers, so the response X-NEM-Sig must be captured via the header event.
 * Match case-insensitively: an intermediary (e.g. Cloudflare) title-cases it to
 * "X-Nem-Sig". */
typedef struct { char sig[45]; bool have; } nem_sig_ctx_t;

static esp_err_t nem_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data
        && evt->header_key && strcasecmp(evt->header_key, "X-NEM-Sig") == 0) {
        nem_sig_ctx_t *ctx = (nem_sig_ctx_t *)evt->user_data;
        size_t n = evt->header_value ? strlen(evt->header_value) : 0;
        if (n > 0 && n < sizeof ctx->sig) {
            memcpy(ctx->sig, evt->header_value, n + 1);
            ctx->have = true;
        }
    }
    return ESP_OK;
}

esp_err_t nem_http_get(const char *url, const nem_auth_t *auth, char *buf, size_t buf_sz, int *out_len)
{
    nem_sig_ctx_t sctx = { .have = false };
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "nem-buddy/0.1",
        .event_handler = nem_http_event,
        .user_data = &sctx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    bool secured = auth && auth->key && auth->device_id && auth->device_id[0];
    if (secured) {
        char b64[45];
        hmac_b64(auth->key, (const uint8_t *)NEM_AUTH_REQ_MSG, strlen(NEM_AUTH_REQ_MSG), b64);
        esp_http_client_set_header(c, "X-NEM-Id", auth->device_id);
        esp_http_client_set_header(c, "X-NEM-Auth", b64);
        esp_http_client_set_header(c, "Accept-Encoding", "identity");
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

    esp_err_t result = (status == 200) ? ESP_OK : ESP_FAIL;
    if (result == ESP_OK && secured) {
        char want[45];
        hmac_b64(auth->key, (const uint8_t *)buf, (size_t)total, want);
        if (!sctx.have || !ct_eq(sctx.sig, want)) {
            ESP_LOGW(TAG, "response signature INVALID — rejecting");
            result = ESP_FAIL;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "GET %s -> %d, %d bytes%s", url, status, total, secured ? " (auth)" : "");
    return result;
}

bool nem_http_auth_selftest(void)
{
    uint8_t mk[32], dk[32];
    mbedtls_sha256((const unsigned char *)"testmaster", 10, mk, 0);      /* master_key */
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), mk, 32,
                    (const uint8_t *)"dev01", 5, dk);                    /* device_key */
    char mac[45], sig[45];
    hmac_b64(dk, (const uint8_t *)NEM_AUTH_REQ_MSG, strlen(NEM_AUTH_REQ_MSG), mac);
    const char *body = "{\"t\":\"2026-07-14T00:00:00\",\"regions\":[]}";
    hmac_b64(dk, (const uint8_t *)body, strlen(body), sig);
    bool ok = ct_eq(mac, "AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=")
           && ct_eq(sig, "uPGokXyiUFAQFM1FjIK8dW3EGa3Rgm23GDpxFIc+VGk=");
    ESP_LOGI(TAG, "auth self-test: %s", ok ? "PASS" : "FAIL");
    return ok;
}
