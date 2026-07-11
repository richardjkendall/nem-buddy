#ifndef WIFI_STA_H
#define WIFI_STA_H
#include "esp_err.h"
esp_err_t wifi_sta_connect(void);   /* blocks until connected or fails */
#endif
