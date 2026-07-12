#ifndef PORTAL_HTTP_H
#define PORTAL_HTTP_H

#include "wifi_ctrl.h"

/* Start the captive-portal HTTP server. `aps`/`ap_n` is the cached scan list
 * shown in the SSID dropdown (borrowed pointer; must outlive the server). A
 * valid POST /save persists creds to NVS and reboots. */
void portal_http_start(const wifi_ctrl_ap_t *aps, int ap_n);

#endif
