/* firmware/main/buttons.c */
#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "btn";

#define BTN_IO18   GPIO_NUM_18
#define POLL_MS    20
#define STABLE_N   2      /* consecutive equal samples = 40ms debounce */

static button_cb_t s_cb;

static void button_task(void *arg)
{
    (void)arg;
    int stable = 1, candidate = 1, count = 0;

    for (;;) {
        int lvl = gpio_get_level(BTN_IO18);
        if (lvl != candidate) { candidate = lvl; count = 0; }
        else if (count < STABLE_N) { count++; }

        if (count >= STABLE_N && candidate != stable) {
            stable = candidate;
            if (stable == 0 && s_cb) {   /* falling edge = pressed */
                ESP_LOGI(TAG, "IO18 pressed");
                s_cb();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(button_cb_t on_press)
{
    s_cb = on_press;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BTN_IO18,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* 8192: on_button (ui_status_toggle) runs on this task's stack and now
     * builds/tears down an entire LVGL overlay under bsp_display_lock(). */
    xTaskCreatePinnedToCore(button_task, "btn", 8192, NULL, 4, NULL, tskNO_AFFINITY);
}
