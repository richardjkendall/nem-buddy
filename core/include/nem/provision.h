#ifndef NEM_PROVISION_H
#define NEM_PROVISION_H

#include <stddef.h>
#include <stdbool.h>

#define NEM_PROV_SSID_MAX   32
#define NEM_PROV_PASS_MAX   64
#define NEM_PROV_URL_MAX    128
#define NEM_PROV_TOKEN_MAX  128

typedef struct {
    char ssid[NEM_PROV_SSID_MAX + 1];
    char password[NEM_PROV_PASS_MAX + 1];
    char proxy_url[NEM_PROV_URL_MAX + 1];
    char proxy_token[NEM_PROV_TOKEN_MAX + 1];
} nem_prov_form_t;

/* Parse an application/x-www-form-urlencoded body. Recognises keys ssid,
 * password, proxy_url, proxy_token; decodes %XX and '+'; ignores unknown keys.
 * Missing fields become "". Returns true iff a non-empty ssid was parsed AND
 * no field's decoded value exceeded its cap. */
bool nem_provision_parse_form(const char *body, size_t len, nem_prov_form_t *out);

/* (Task 2 adds nem_provision_build_dns_reply here.) */

#endif
