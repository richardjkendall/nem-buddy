#ifndef NEM_HISTORY_H
#define NEM_HISTORY_H

#include <stdbool.h>
#include "nem/snapshot.h"

#define NEM_HISTORY_SLOTS 288  /* 5-minute slots across 24h */

typedef struct {
    double price[NEM_HISTORY_SLOTS];
    double demand[NEM_HISTORY_SLOTS];
    bool   filled[NEM_HISTORY_SLOTS];
    int    day_index;
} nem_region_history_t;

typedef struct {
    nem_region_history_t regions[NEM_REGION_COUNT];
} nem_history_t;

void nem_history_init(nem_history_t *h);
void nem_history_add(nem_history_t *h, const nem_snapshot_t *snap, long long epoch);
int  nem_history_filled_count(const nem_region_history_t *rh);

#endif
