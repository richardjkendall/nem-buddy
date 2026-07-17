/* firmware/main/axp2101.h */
#ifndef AXP2101_H
#define AXP2101_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     present;      /* a cell is connected */
    bool     charging;     /* actively charging */
    uint8_t  percent;      /* 0..100 from the fuel gauge */
    uint16_t millivolts;   /* battery terminal voltage */
} axp2101_state_t;

/* Log every responding address on the BSP's shared I2C bus, then the chip ID. */
void axp2101_scan_log(void);

/* Probe the PMIC and enable the battery voltage ADC. Call once at boot, after
 * the BSP has brought I2C up. Returns ESP_OK only if the chip answered with the
 * expected ID; on failure the device is treated as absent and axp2101_read()
 * will fail cleanly rather than block. */
esp_err_t axp2101_init(void);

/* Fill `out` from the PMIC. Returns ESP_ERR_INVALID_STATE if init failed. */
esp_err_t axp2101_read(axp2101_state_t *out);

#endif
