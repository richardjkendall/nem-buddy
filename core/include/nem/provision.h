#ifndef NEM_PROVISION_H
#define NEM_PROVISION_H

#include <stddef.h>
#include <stdbool.h>

#define NEM_PROV_SSID_MAX   32
#define NEM_PROV_PASS_MAX   64
#define NEM_PROV_URL_MAX    128
#define NEM_PROV_TOKEN_MAX  128
#define NEM_PROV_DEVID_MAX  64

typedef struct {
    char ssid[NEM_PROV_SSID_MAX + 1];
    char password[NEM_PROV_PASS_MAX + 1];
    char proxy_url[NEM_PROV_URL_MAX + 1];
    char proxy_token[NEM_PROV_TOKEN_MAX + 1];
    char device_id[NEM_PROV_DEVID_MAX + 1];
} nem_prov_form_t;

/* Parse an application/x-www-form-urlencoded body. Recognises keys ssid,
 * password, proxy_url, proxy_token, device_id; decodes %XX and '+'; ignores unknown keys.
 * Missing fields become "". Returns true iff a non-empty ssid was parsed AND
 * no field's decoded value exceeded its cap. */
bool nem_provision_parse_form(const char *body, size_t len, nem_prov_form_t *out);

/* Build a DNS response answering the single-question query in `query` with an
 * A record pointing at ip[4]. Writes into out[0..out_cap). Returns the reply
 * length, or -1 if the query is too short or the buffer is too small. */
int nem_provision_build_dns_reply(const unsigned char *query, int qlen,
                                  const unsigned char ip[4],
                                  unsigned char *out, int out_cap);

#endif
