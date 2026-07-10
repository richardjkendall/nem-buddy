#include "nem/history.h"
#include "nem/timeutil.h"
#include <string.h>

void nem_history_init(nem_history_t *h) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < NEM_REGION_COUNT; i++) h->regions[i].day_index = -1;
}

static void reset_region(nem_region_history_t *rh, int day) {
    memset(rh->price, 0, sizeof(rh->price));
    memset(rh->demand, 0, sizeof(rh->demand));
    memset(rh->filled, 0, sizeof(rh->filled));
    rh->day_index = day;
}

void nem_history_add(nem_history_t *h, const nem_snapshot_t *snap, long long epoch) {
    int day = nem_day_index(epoch);
    int slot = nem_minute_of_day(epoch) / 5;
    if (slot < 0 || slot >= NEM_HISTORY_SLOTS) return;

    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        const nem_region_snapshot_t *rs = &snap->regions[i];
        if (!rs->valid) continue;
        nem_region_history_t *rh = &h->regions[i];
        if (rh->day_index != day) reset_region(rh, day);
        rh->price[slot] = rs->price;
        rh->demand[slot] = rs->demand_mw;
        rh->filled[slot] = true;
    }
}

int nem_history_filled_count(const nem_region_history_t *rh) {
    int n = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) if (rh->filled[i]) n++;
    return n;
}
