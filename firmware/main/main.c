#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui_theme.h"

static const char *TAG = "nem-buddy";
static int s_tap_count = 0;
static lv_obj_t *s_tap_label;

static void tap_cb(lv_event_t *e)
{
    (void)e;
    s_tap_count++;
    lv_label_set_text_fmt(s_tap_label, "taps: %d", s_tap_count);
    ESP_LOGI(TAG, "touch tap #%d", s_tap_count);
}

void app_main(void)
{
    ESP_LOGI(TAG, "NEM Buddy bring-up starting");
    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_brightness_set(80);

    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "NEM Buddy");
    lv_obj_set_style_text_color(title, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -28);

    s_tap_label = lv_label_create(scr);
    lv_label_set_text(s_tap_label, "taps: 0");
    lv_obj_set_style_text_color(s_tap_label, NEM_C_BLUE, 0);
    lv_obj_set_style_text_font(s_tap_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_tap_label, LV_ALIGN_CENTER, 0, 24);

    /* Screen-level click proves the touch pipeline end to end. */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);

    bsp_display_unlock();
    ESP_LOGI(TAG, "UI up; tap the screen to test touch");
}
