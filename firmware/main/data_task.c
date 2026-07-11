#include "data_task.h"
#include "wifi_sta.h"
#include "net_fetch.h"
#include "ui_dashboard.h"
#include "secrets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "nem/proxy_client.h"
#include "nem/config.h"

static const char *TAG = "data";
#define PROXY_BUF_SZ (8 * 1024)

static void data_task(void *arg)
{
    (void)arg;
    /* Let the initial dashboard fully render before WiFi init grabs internal DMA RAM. */
    vTaskDelay(pdMS_TO_TICKS(700));

    if (wifi_sta_connect() != ESP_OK) { ESP_LOGE(TAG, "no wifi; task exiting"); vTaskDelete(NULL); return; }

    nem_config_t cfg; nem_config_defaults(&cfg);
    char *buf = heap_caps_malloc(PROXY_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no PSRAM buffer"); vTaskDelete(NULL); return; }

    for (;;) {
        int len = 0;
        if (nem_http_get(NEM_PROXY_URL, NULL, buf, PROXY_BUF_SZ, &len) == ESP_OK) {
            nem_snapshot_t snap;
            nem_region_mix_t mix;
            if (nem_proxy_parse(buf, &snap, &mix)) {
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                const nem_fuel_mix_t *hm = &mix.regions[cfg.home_region];
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, hm, cfg.home_region);
                bsp_display_unlock();
                ESP_LOGI(TAG, "ok: %s $%.1f  demand %.0f  ren %.0f%%",
                         nem_region_name(cfg.home_region), h->price, h->demand_mw,
                         hm->renewable_fraction * 100.0);
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
