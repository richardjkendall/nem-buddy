#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui_dashboard.h"
#include "net_manager.h"

static const char *TAG = "nem-buddy";

void app_main(void)
{
    ESP_LOGI(TAG, "NEM Buddy starting");
    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_brightness_set(80);

    bsp_display_lock(-1);
    ui_dashboard_create(lv_screen_active());
    bsp_display_unlock();

    net_manager_start();
}
