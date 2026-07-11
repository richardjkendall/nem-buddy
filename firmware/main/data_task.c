#include "data_task.h"
#include "wifi_sta.h"
#include "net_fetch.h"
#include "ui_dashboard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "nem/aemo_client.h"
#include "nem/history.h"
#include "nem/config.h"

static const char *TAG = "data";
#define AEMO_URL "https://visualisations.aemo.com.au/aemo/apps/api/report/ELEC_NEM_SUMMARY"
#define AEMO_BUF_SZ (32 * 1024)

static void data_task(void *arg)
{
    (void)arg;
    if (wifi_sta_connect() != ESP_OK) { ESP_LOGE(TAG, "no wifi; task exiting"); vTaskDelete(NULL); return; }

    nem_config_t cfg; nem_config_defaults(&cfg);
    static nem_history_t history;  /* ~24KB: static, NOT on the task stack */
    nem_history_init(&history);

    char *buf = heap_caps_malloc(AEMO_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no PSRAM buffer"); vTaskDelete(NULL); return; }

    for (;;) {
        int len = 0;
        if (nem_http_get(AEMO_URL, NULL, buf, AEMO_BUF_SZ, &len) == ESP_OK) {
            nem_snapshot_t snap;
            if (nem_aemo_parse_summary(buf, &snap)) {
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                if (h->valid) nem_history_add(&history, &snap, h->settlement_epoch);
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, cfg.home_region);
                bsp_display_unlock();
                ESP_LOGI(TAG, "AEMO ok: %s $%.1f  demand %.0f",
                         nem_region_name(cfg.home_region), h->price, h->demand_mw);
            } else {
                ESP_LOGW(TAG, "AEMO parse failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

void data_task_start(void)
{
    xTaskCreatePinnedToCore(data_task, "data", 8192, NULL, 5, NULL, tskNO_AFFINITY);
}
