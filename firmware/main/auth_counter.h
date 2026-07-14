#ifndef AUTH_COUNTER_H
#define AUTH_COUNTER_H
/* Monotonic request counter, persisted in NVS with a reservation gap so it
 * survives reboots (always jumps forward) with ~1 NVS write per 1024 calls. */
unsigned long long auth_counter_next(void);
#endif
