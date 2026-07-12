#include "data_task.h"
#include "net_creds.h"
#include "net_fetch.h"
#include "ui_dashboard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "nem/proxy_client.h"
#include "nem/config.h"
#include "nem/history.h"

static const char *TAG = "data";
#define PROXY_BUF_SZ (8 * 1024)

static nem_history_t *s_hist;

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
    const char *bearer = creds.proxy_token[0] ? creds.proxy_token : NULL;

    nem_config_t cfg; nem_config_defaults(&cfg);
    char *buf = heap_caps_malloc(PROXY_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no PSRAM buffer"); vTaskDelete(NULL); return; }

    s_hist = heap_caps_malloc(sizeof(nem_history_t), MALLOC_CAP_SPIRAM);
    if (!s_hist) { ESP_LOGE(TAG, "no PSRAM history"); vTaskDelete(NULL); return; }
    nem_history_init(s_hist);

    for (;;) {
        int len = 0;
        if (nem_http_get(creds.proxy_url, bearer, buf, PROXY_BUF_SZ, &len) == ESP_OK) {
            nem_snapshot_t snap;
            nem_region_mix_t mix;
            if (nem_proxy_parse(buf, &snap, &mix)) {
                long long epoch = snap.regions[cfg.home_region].settlement_epoch;
                if (epoch > 0) nem_history_add(s_hist, &snap, epoch);
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, &mix);
                bsp_display_unlock();
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                ESP_LOGI(TAG, "ok: %s $%.1f  demand %.0f",
                         nem_region_name(cfg.home_region), h->price, h->demand_mw);
            } else {
                ESP_LOGW(TAG, "proxy parse failed (%d bytes)", len);
            }
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
