#ifndef WIFI_CTRL_H
#define WIFI_CTRL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "net_creds.h"

#define WIFI_CTRL_SCAN_MAX 16

typedef struct {
    char   ssid[33];
    int8_t rssi;
} wifi_ctrl_ap_t;

/* One-time init of nvs_flash, netif, default event loop, STA+AP netifs, and
 * esp_wifi. Does NOT start wifi. Idempotent. */
esp_err_t wifi_ctrl_init(void);

/* Connect as STA using creds; blocks until GOT_IP or `max_retries` disconnects.
 * Returns ESP_OK on success, ESP_FAIL otherwise. */
esp_err_t wifi_ctrl_sta_connect(const net_creds_t *c, int max_retries);

/* Bring up the provisioning radio: APSTA mode, scan nearby APs (fills scan_out /
 * *scan_n up to WIFI_CTRL_SCAN_MAX), and start WPA2 SoftAP `ap_ssid` at
 * 192.168.4.1. */
esp_err_t wifi_ctrl_portal_start(const char *ap_ssid, const char *ap_pass,
                                 wifi_ctrl_ap_t *scan_out, int *scan_n);

/* Named wifi_ctrl_sta_info_t (not wifi_sta_info_t) — the latter is already a
 * system typedef in esp_wifi_types_generic.h (AP-mode connected-station info)
 * and collides at compile time. */
typedef struct {
    bool   connected;
    char   ssid[33];
    int8_t rssi;
    char   ip[16];      /* dotted quad, empty when no IP */
} wifi_ctrl_sta_info_t;

/* Snapshot the STA link. Always fills `out`; sets connected=false and leaves
 * ssid/ip empty when not associated. Safe to call from any task. */
void wifi_ctrl_sta_info(wifi_ctrl_sta_info_t *out);

#endif
