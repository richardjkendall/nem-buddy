#ifndef NEM_SNAPSHOT_H
#define NEM_SNAPSHOT_H

#include <stdbool.h>
#include "nem/regions.h"

#define NEM_MAX_INTERCONNECTORS 6

typedef struct {
    char   name[24];
    double value;   /* MW; sign indicates direction */
} nem_interconnector_flow_t;

typedef struct {
    nem_region_t region;
    bool         valid;
    double       price;            /* $/MWh (RRP)     */
    double       demand_mw;        /* TOTALDEMAND     */
    double       net_interchange;  /* NETINTERCHANGE  */
    long long    settlement_epoch; /* unix seconds    */
    int          interconnector_count;
    nem_interconnector_flow_t interconnectors[NEM_MAX_INTERCONNECTORS];
} nem_region_snapshot_t;

typedef struct {
    nem_region_snapshot_t regions[NEM_REGION_COUNT];
} nem_snapshot_t;

#endif
