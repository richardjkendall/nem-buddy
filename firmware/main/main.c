#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui_dashboard.h"
#include "net_manager.h"
#include "axp2101.h"
#include "buttons.h"
#include "ui_status.h"

static const char *TAG = "nem-buddy";

static void on_button(void)
{
    bsp_display_lock(-1);          /* button task must never touch LVGL unlocked */
    ui_status_toggle();
    bsp_display_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "NEM Buddy starting");
    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_brightness_set(80);

    bsp_display_lock(-1);
    ui_dashboard_create(lv_screen_active());
    bsp_display_unlock();

    if (axp2101_init() != ESP_OK)
        ESP_LOGW(TAG, "battery monitoring unavailable");

    buttons_start(on_button);

    net_manager_start();
}
