#include "nem/battery.h"
#include <stddef.h>

/* Piecewise-linear approximation of a 3.7V Li-ion discharge curve. The curve is
 * flat through the middle and steep at the ends, which is why a lookup beats a
 * straight line. */
static const struct { uint16_t mv; uint8_t pct; } CURVE[] = {
    { 3300,   0 }, { 3600,  10 }, { 3700,  25 }, { 3750,  40 },
    { 3800,  55 }, { 3870,  70 }, { 3950,  85 }, { 4100,  95 },
    { 4200, 100 },
};
#define N (sizeof CURVE / sizeof CURVE[0])

uint8_t nem_batt_pct_from_mv(uint16_t mv)
{
    if (mv <= CURVE[0].mv)     return CURVE[0].pct;
    if (mv >= CURVE[N - 1].mv) return CURVE[N - 1].pct;

    for (size_t i = 1; i < N; i++) {
        if (mv <= CURVE[i].mv) {
            uint16_t span = CURVE[i].mv  - CURVE[i - 1].mv;
            uint8_t  rise = CURVE[i].pct - CURVE[i - 1].pct;
            uint16_t into = mv - CURVE[i - 1].mv;
            return (uint8_t)(CURVE[i - 1].pct + (into * rise + span / 2) / span);
        }
    }
    return CURVE[N - 1].pct;   /* unreachable */
}
