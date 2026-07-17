#ifndef NEM_BATTERY_H
#define NEM_BATTERY_H

#include <stdint.h>

/* Estimate charge percentage (0..100) from a single-cell Li-ion terminal
 * voltage in millivolts, using a piecewise-linear discharge curve.
 *
 * This is a fallback and a sanity check for the AXP2101's own fuel gauge, which
 * depends on an internal battery model and can read oddly on a cell it has not
 * characterised. It is deliberately approximate: terminal voltage sags under
 * load, so a resting cell and a working one read differently at the same charge.
 * Clamped at both rails. */
uint8_t nem_batt_pct_from_mv(uint16_t mv);

#endif
