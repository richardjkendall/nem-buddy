#ifndef NET_MANAGER_H
#define NET_MANAGER_H

/* Spawn the boot state machine: load creds -> STA connect, or fall back to the
 * SoftAP captive portal. On success it starts the data task. Call once from
 * app_main after the dashboard shell is created. */
void net_manager_start(void);

#endif
