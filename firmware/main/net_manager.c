#include "net_manager.h"
#include "wifi_ctrl.h"
#include "net_creds.h"
#include "captive_dns.h"
#include "portal_http.h"
#include "ui_setup.h"
#include "data_task.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#ifndef NEM_SETUP_AP_PASSWORD
#define NEM_SETUP_AP_PASSWORD "nembuddy"   /* WPA2 needs >= 8 chars */
#endif
#define STA_MAX_RETRIES 5

static const char *TAG = "net";
static wifi_ctrl_ap_t s_aps[WIFI_CTRL_SCAN_MAX];

static void net_task(void *arg) {
    (void)arg;
    /* Let the first dashboard render settle before WiFi grabs internal DMA RAM. */
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_ERROR_CHECK(wifi_ctrl_init());

    net_creds_t creds;
    bool have = net_creds_load(&creds);
    if (have && wifi_ctrl_sta_connect(&creds, STA_MAX_RETRIES) == ESP_OK) {
        ESP_LOGI(TAG, "connected; starting data task");
        data_task_start();
        vTaskDelete(NULL);
        return;
    }

    /* PORTAL */
    ESP_LOGW(TAG, "no creds or connect failed; entering setup portal");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap[32];
    snprintf(ap, sizeof ap, "NEM-Buddy-%02X%02X", mac[4], mac[5]);

    bsp_display_lock(-1);
    ui_setup_show(ap, NEM_SETUP_AP_PASSWORD, "http://192.168.4.1");
    bsp_display_unlock();

    int n = 0;
    if (wifi_ctrl_portal_start(ap, NEM_SETUP_AP_PASSWORD, s_aps, &n) != ESP_OK) {
        ESP_LOGE(TAG, "portal radio bring-up failed");
        bsp_display_lock(-1);
        ui_setup_status("Setup radio failed \xE2\x80\x94 restart device");
        bsp_display_unlock();
        vTaskDelete(NULL);
        return;
    }
    captive_dns_start();
    portal_http_start(s_aps, n);
    ESP_LOGI(TAG, "portal ready (%d APs); awaiting setup", n);
    vTaskDelete(NULL);   /* server + DNS tasks keep running; reboot on save */
}

void net_manager_start(void) {
    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 5, NULL, tskNO_AFFINITY);
}
