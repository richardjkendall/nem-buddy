/* firmware/main/axp2101.c */
#include "axp2101.h"
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "axp";

/* Datasheet values — UNVERIFIED on this board until this task runs. */
#define AXP2101_ADDR      0x34
#define AXP2101_REG_CHIP  0x03
#define AXP2101_CHIP_ID   0x4A

void axp2101_scan_log(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) { ESP_LOGE(TAG, "no BSP I2C bus handle"); return; }

    char line[128];
    int n = snprintf(line, sizeof line, "I2C scan:");
    for (uint8_t a = 0x08; a < 0x78 && n > 0 && n < (int)sizeof line; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK)
            n += snprintf(line + n, sizeof line - n, " 0x%02X", a);
    }
    ESP_LOGI(TAG, "%s", line);

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed", AXP2101_ADDR);
        return;
    }
    uint8_t reg = AXP2101_REG_CHIP, id = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 200);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no reply at 0x%02X: %s", AXP2101_ADDR, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "chip id at 0x%02X = 0x%02X (expect 0x%02X) -> %s",
                 AXP2101_ADDR, id, AXP2101_CHIP_ID,
                 id == AXP2101_CHIP_ID ? "MATCH" : "MISMATCH");
    }
    i2c_master_bus_rm_device(dev);
}
