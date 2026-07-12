#ifndef UI_SETUP_H
#define UI_SETUP_H

/* Replace the active screen with a Wi-Fi setup card. Call inside
 * bsp_display_lock()/unlock(). */
void ui_setup_show(const char *ap_ssid, const char *ap_pass, const char *portal_url);

/* Update the status line on the setup card. Call inside the display lock. */
void ui_setup_status(const char *msg);

#endif
