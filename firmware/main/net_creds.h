#ifndef NET_CREDS_H
#define NET_CREDS_H

#include <stdbool.h>
#include "nem/provision.h"

/* Same shape as the parsed form: ssid/password/proxy_url/proxy_token/device_id. */
typedef nem_prov_form_t net_creds_t;

/* Load creds from NVS namespace "nem". If NVS has no ssid, seed from secrets.h
 * (if that header is present at build time). Returns true iff a non-empty ssid
 * is available. */
bool net_creds_load(net_creds_t *out);

/* Persist creds to NVS namespace "nem". Returns true on success. */
bool net_creds_save(const net_creds_t *c);

#endif
