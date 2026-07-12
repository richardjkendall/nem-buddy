idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server)
