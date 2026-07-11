idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif)
