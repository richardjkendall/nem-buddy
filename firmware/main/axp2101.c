/* firmware/main/axp2101.c */
#include "axp2101.h"
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "axp";

/* Verified on this board in Task 1: chip answers at 0x34 with chip ID 0x4A. */
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

#define REG_STATUS1   0x00
#define REG_STATUS2   0x01
#define REG_ADC_EN    0x30
#define REG_VBAT_H    0x34
#define REG_PERCENT   0xA4

static i2c_master_dev_handle_t s_dev;
static bool s_ok;

static esp_err_t rd(uint8_t reg, uint8_t *val, size_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, n, 200);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(s_dev, b, sizeof b, 200);
}

esp_err_t axp2101_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) { ESP_LOGE(TAG, "no BSP I2C bus"); return ESP_ERR_INVALID_STATE; }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "PMIC not addressable; battery reports unavailable");
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t id = 0;
    if (rd(AXP2101_REG_CHIP, &id, 1) != ESP_OK || id != AXP2101_CHIP_ID) {
        ESP_LOGW(TAG, "PMIC id 0x%02X != 0x%02X; battery reports unavailable",
                 id, AXP2101_CHIP_ID);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        axp2101_scan_log();   /* the one failure where a bus dump is worth having */
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t adc = 0;
    if (rd(REG_ADC_EN, &adc, 1) == ESP_OK)
        wr(REG_ADC_EN, adc | 0x01);      /* battery voltage ADC on */

    s_ok = true;
    ESP_LOGI(TAG, "AXP2101 ready");
    return ESP_OK;
}

esp_err_t axp2101_read(axp2101_state_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (axp2101_state_t){ 0 };
    if (!s_ok) return ESP_ERR_INVALID_STATE;

    uint8_t s1 = 0, s2 = 0, pct = 0, v[2] = { 0 };
    if (rd(REG_STATUS1, &s1, 1) != ESP_OK) return ESP_FAIL;
    if (rd(REG_STATUS2, &s2, 1) != ESP_OK) return ESP_FAIL;

    out->present  = (s1 & 0x08) != 0;
    out->charging = ((s2 >> 5) & 0x03) == 0x01;

    if (rd(REG_PERCENT, &pct, 1) == ESP_OK && pct <= 100) out->percent = pct;
    if (rd(REG_VBAT_H, v, 2) == ESP_OK)
        out->millivolts = (uint16_t)(((v[0] & 0x3F) << 8) | v[1]);

    return ESP_OK;
}
