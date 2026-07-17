/* firmware/main/ui_status.h */
#ifndef UI_STATUS_H
#define UI_STATUS_H

#include <stdbool.h>

/* Toggle the device status overlay. Must be called inside
 * bsp_display_lock(-1) / bsp_display_unlock(). */
void ui_status_toggle(void);

bool ui_status_is_open(void);

#endif
