#ifndef DATA_TASK_H
#define DATA_TASK_H
#include "nem/regions.h"
#include "nem/history.h"

void data_task_start(void);   /* polls + updates the UI (WiFi already up) */

/* Latest intraday history for a region (RAM-only, accumulated from boot).
 * NULL until the first poll allocates it. Read inside bsp_display_lock(). */
const nem_region_history_t *nem_history_of(nem_region_t region);

#endif
