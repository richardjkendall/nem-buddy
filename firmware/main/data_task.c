#include "data_task.h"
#include "net_creds.h"
#include "net_fetch.h"
#include "ui_dashboard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "nem/proxy_client.h"
#include "nem/config.h"
#include "nem/history.h"
#include "ui_drill.h"
#include "mbedtls/base64.h"
#include "nem/proxy_auth.h"
#include <string.h>

static const char *TAG = "data";
#define PROXY_BUF_SZ (24 * 1024)   /* holds the intraday history curves too */

static nem_history_t *s_hist;
static long long s_last_epoch = 0;
static long long s_last_ok_s = -1;
static uint32_t  s_consec_errors;

static long long uptime_s(void) { return esp_timer_get_time() / 1000000; }

static void data_task(void *arg)
{
    (void)arg;
    net_creds_t creds;
    net_creds_load(&creds);
    if (creds.proxy_url[0] == '\0') {
        ESP_LOGW(TAG, "no proxy configured; data task idle");
        vTaskDelete(NULL);
        return;
    }
    uint8_t auth_key[32];
    size_t klen = 0;
    bool secured = creds.device_id[0] != '\0' && creds.proxy_token[0] != '\0'
        && mbedtls_base64_decode(auth_key, sizeof auth_key, &klen,
               (const unsigned char *)creds.proxy_token, strlen(creds.proxy_token)) == 0
        && klen == 32;
    nem_http_auth_selftest();

    nem_config_t cfg; nem_config_defaults(&cfg);
    char *buf = heap_caps_malloc(PROXY_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no PSRAM buffer"); vTaskDelete(NULL); return; }

    s_hist = heap_caps_malloc(sizeof(nem_history_t), MALLOC_CAP_SPIRAM);
    if (!s_hist) { ESP_LOGE(TAG, "no PSRAM history"); vTaskDelete(NULL); return; }
    nem_history_init(s_hist);

    for (;;) {
        int len = 0;
        nem_auth_t auth = { .key = secured ? auth_key : NULL, .device_id = secured ? creds.device_id : NULL };
        if (nem_http_get(creds.proxy_url, &auth, buf, PROXY_BUF_SZ, &len) == ESP_OK) {
            nem_snapshot_t snap;
            nem_region_mix_t mix;
            if (nem_proxy_parse(buf, &snap, &mix)) {
                long long ep = snap.regions[cfg.home_region].settlement_epoch;
                if (!nem_auth_accept_fresh(ep, s_last_epoch)) {
                    ESP_LOGW(TAG, "stale/replayed payload (epoch %lld <= %lld) — dropped", ep, s_last_epoch);
                    s_consec_errors++;
                    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
                    continue;
                }
                s_last_epoch = ep;
                s_last_ok_s = uptime_s();
                s_consec_errors = 0;
                bsp_display_lock(-1);
                nem_proxy_parse_history(buf, s_hist);   /* today's curve from the proxy */
                ui_dashboard_update(&snap, &mix);
                ui_drill_refresh();
                bsp_display_unlock();
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                ESP_LOGI(TAG, "ok: %s $%.1f  demand %.0f",
                         nem_region_name(cfg.home_region), h->price, h->demand_mw);
            } else {
                ESP_LOGW(TAG, "proxy parse failed (%d bytes)", len);
                s_consec_errors++;
            }
        } else {
            s_consec_errors++;
            ESP_LOGW(TAG, "fetch failed");
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

void data_task_start(void)
{
    xTaskCreatePinnedToCore(data_task, "data", 8192, NULL, 5, NULL, tskNO_AFFINITY);
}

const nem_region_history_t *nem_history_of(nem_region_t region)
{
    if (!s_hist || region >= NEM_REGION_COUNT) return NULL;
    return &s_hist->regions[region];
}

void data_task_health(data_health_t *out)
{
    if (!out) return;
    out->uptime_s      = uptime_s();
    out->last_ok_s     = s_last_ok_s;
    out->consec_errors = s_consec_errors;
}
