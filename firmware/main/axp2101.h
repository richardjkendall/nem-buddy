/* firmware/main/axp2101.h */
#ifndef AXP2101_H
#define AXP2101_H

/* Log every responding address on the BSP's shared I2C bus, then read and log
 * the AXP2101's chip ID. Diagnostic only — safe to call once at boot, after
 * the BSP has brought the I2C bus up. */
void axp2101_scan_log(void);

#endif
