#include "wifi_ctrl.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
static int  s_retries, s_max_retries;
static bool s_inited;
static bool s_want_sta;   /* only drive STA connect/retry when a STA connect was requested */

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_sta) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_want_sta) {
            return;
        }
        if (s_retries < s_max_retries) {
            s_retries++; esp_wifi_connect(); ESP_LOGW(TAG, "retry %d", s_retries);
        } else {
            xEventGroupSetBits(s_events, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t wifi_ctrl_init(void) {
    if (s_inited) return ESP_OK;
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL));
    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_ctrl_sta_connect(const net_creds_t *c, int max_retries) {
    s_want_sta = true;
    s_retries = 0;
    s_max_retries = max_retries;
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid,     c->ssid,     sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, c->password, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", c->ssid);

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_ctrl_portal_start(const char *ap_ssid, const char *ap_pass,
                                 wifi_ctrl_ap_t *scan_out, int *scan_n) {
    s_want_sta = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    *scan_n = 0;
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t num = WIFI_CTRL_SCAN_MAX;
        wifi_ap_record_t recs[WIFI_CTRL_SCAN_MAX];
        if (esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
            int n = 0;
            for (int i = 0; i < (int)num && n < WIFI_CTRL_SCAN_MAX; i++) {
                if (recs[i].ssid[0] == 0) continue;
                strlcpy(scan_out[n].ssid, (char *)recs[i].ssid, sizeof scan_out[n].ssid);
                scan_out[n].rssi = recs[i].rssi;
                n++;
            }
            *scan_n = n;
        }
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid,     ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap.ap.password, ap_pass, sizeof ap.ap.password);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    if (strlen(ap_pass) < 8) {
        ESP_LOGE(TAG, "AP password too short for WPA2 (>= 8 chars)");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t aerr = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "AP set_config failed: %s", esp_err_to_name(aerr));
        return aerr;
    }
    ESP_LOGI(TAG, "SoftAP \"%s\" up at 192.168.4.1 (%d APs scanned)", ap_ssid, *scan_n);
    return ESP_OK;
}
