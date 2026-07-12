#include "nem/provision.h"
#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)(c | 0x20);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* URL-decode s[0..n) into out (NUL-terminated, capped at cap-1 bytes). */
static void urldecode(const char *s, int n, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < cap; i++) {
        char c = s[i];
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && i + 2 < n) {
            int h = hexval(s[i + 1]), l = hexval(s[i + 2]);
            if (h >= 0 && l >= 0) { out[o++] = (char)(h * 16 + l); i += 2; }
            else out[o++] = c;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

bool nem_provision_parse_form(const char *body, size_t len, nem_prov_form_t *out) {
    memset(out, 0, sizeof(*out));
    if (!body) return false;
    const char *p = body, *end = body + len;
    bool over = false;

    struct { const char *key; char *dst; size_t cap; } fields[] = {
        { "ssid",        out->ssid,        NEM_PROV_SSID_MAX  },
        { "password",    out->password,    NEM_PROV_PASS_MAX  },
        { "proxy_url",   out->proxy_url,   NEM_PROV_URL_MAX   },
        { "proxy_token", out->proxy_token, NEM_PROV_TOKEN_MAX },
    };

    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        if (!amp) amp = end;
        const char *eq = memchr(p, '=', (size_t)(amp - p));
        const char *kb = p, *ke = eq ? eq : amp;
        const char *vb = eq ? eq + 1 : amp, *ve = amp;

        char key[32], val[256];
        urldecode(kb, (int)(ke - kb), key, sizeof key);
        urldecode(vb, (int)(ve - vb), val, sizeof val);
        size_t vlen = strlen(val);

        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            if (strcmp(key, fields[i].key) == 0) {
                if (vlen > fields[i].cap) over = true;
                else memcpy(fields[i].dst, val, vlen + 1);
            }
        }
        p = amp + 1;
    }
    return out->ssid[0] != '\0' && !over;
}

int nem_provision_build_dns_reply(const unsigned char *query, int qlen,
                                  const unsigned char ip[4],
                                  unsigned char *out, int out_cap) {
    if (!query || !out || qlen < 12) return -1;
    if (qlen + 16 > out_cap) return -1;

    memcpy(out, query, (size_t)qlen);
    out[2] = (unsigned char)(query[2] | 0x80);  /* set QR, keep opcode/RD    */
    out[3] = 0x80;                              /* RA=1, RCODE=0             */
    out[6] = 0x00; out[7] = 0x01;               /* ANCOUNT = 1               */
    out[8] = 0x00; out[9] = 0x00;               /* NSCOUNT = 0               */
    out[10] = 0x00; out[11] = 0x00;             /* ARCOUNT = 0               */

    unsigned char *p = out + qlen;
    *p++ = 0xC0; *p++ = 0x0C;                    /* name -> question at off 12 */
    *p++ = 0x00; *p++ = 0x01;                    /* TYPE A                     */
    *p++ = 0x00; *p++ = 0x01;                    /* CLASS IN                   */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C;  /* TTL 60            */
    *p++ = 0x00; *p++ = 0x04;                    /* RDLENGTH 4                 */
    *p++ = ip[0]; *p++ = ip[1]; *p++ = ip[2]; *p++ = ip[3];
    return (int)(p - out);
}
