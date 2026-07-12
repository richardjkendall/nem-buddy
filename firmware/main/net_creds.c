#include "net_creds.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

/* secrets.h is optional (gitignored). Include it only if present so a fresh
 * checkout without it still builds and goes through the portal. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#define NS "nem"

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap) {
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

bool net_creds_load(net_creds_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, "ssid", out->ssid,        sizeof out->ssid);
        load_str(h, "pass", out->password,    sizeof out->password);
        load_str(h, "url",  out->proxy_url,   sizeof out->proxy_url);
        load_str(h, "tok",  out->proxy_token, sizeof out->proxy_token);
        nvs_close(h);
    }
    if (out->ssid[0] == '\0') {
#ifdef NEM_WIFI_SSID
        strlcpy(out->ssid,     NEM_WIFI_SSID,     sizeof out->ssid);
        strlcpy(out->password, NEM_WIFI_PASSWORD, sizeof out->password);
#endif
#ifdef NEM_PROXY_URL
        if (out->proxy_url[0] == '\0')
            strlcpy(out->proxy_url, NEM_PROXY_URL, sizeof out->proxy_url);
#endif
    }
    return out->ssid[0] != '\0';
}

bool net_creds_save(const net_creds_t *c) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", c->ssid)        == ESP_OK
           && nvs_set_str(h, "pass", c->password)    == ESP_OK
           && nvs_set_str(h, "url",  c->proxy_url)   == ESP_OK
           && nvs_set_str(h, "tok",  c->proxy_token) == ESP_OK
           && nvs_commit(h)                          == ESP_OK;
    nvs_close(h);
    return ok;
}
