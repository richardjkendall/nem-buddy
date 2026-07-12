# NEM Buddy — WiFi Provisioning (Plan 3b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the compile-time WiFi/proxy config in `firmware/main/secrets.h` with an on-device SoftAP captive-portal that captures WiFi + proxy settings, persists them to NVS, and connects.

**Architecture:** A boot state machine (`net_manager`) loads creds from NVS (seeding from `secrets.h` on first boot if present). If creds exist and STA connect succeeds, it starts the data task. Otherwise it enters a WPA2 SoftAP + captive portal (DNS hijack + HTTP form + on-screen setup card); on form submit it saves to NVS and reboots into the connect path. All parsing/DNS logic is pure C in `core/` (host-tested with Unity); the ESP-IDF glue is compile-gated and validated on-board.

**Tech Stack:** ESP-IDF v5.5, LVGL v9, target esp32s3, FreeRTOS, esp_wifi (APSTA), esp_http_server, lwIP sockets (DNS), NVS. Host tests: CMake + CTest + vendored Unity.

**Design spec:** `docs/superpowers/specs/2026-07-12-nem-buddy-wifi-provisioning-design.md`

## Global Constraints

- **Toolchain (isolated, outside repo):** every firmware shell must run `source ~/esp/idf-env.sh` first. Build: `idf.py -C firmware build`. Flashing is done by the human (board at `/dev/cu.usbmodem21101`); agents do not flash.
- **Targets/versions:** ESP-IDF **v5.5**, LVGL **v9**, target **esp32s3**. Do not attempt on-device TLS (RAM constraint — device reads the plain-HTTP proxy).
- **Core is the single source of truth for portable C.** Every new `core/src/*.c` must be registered in **both** `core/CMakeLists.txt` (host tests) **and** `firmware/components/nem_core/CMakeLists.txt` (firmware build).
- **Core builds `-Wall -Wextra -Werror`** — no warnings allowed in `core/`.
- **LVGL calls only between `bsp_display_lock(-1)` / `bsp_display_unlock()`.** `lv_color_hex()` is not a constant initializer — only use it at runtime, never in a file-scope table.
- **Host test cycle:** `cmake --build core/build && ctest --test-dir core/build`. If `core/build` does not exist yet, create it once with `cmake -S core -B core/build`.
- **Setup AP password** is the compile-time constant `NEM_SETUP_AP_PASSWORD` (default `"nembuddy"`, ≥8 chars, shown on the setup card).

---

## File Structure

**Core (host-tested, portable C):**
- `core/include/nem/provision.h` — form-parse + DNS-reply interfaces (Tasks 1–2)
- `core/src/provision.c` — implementations (Tasks 1–2)
- `core/test/test_provision.c` — Unity tests (Tasks 1–2)

**Firmware glue (`firmware/main/`):**
- `net_creds.{h,c}` — NVS load/save + `secrets.h` seed (Task 3)
- `wifi_ctrl.{h,c}` — wifi/netif init, STA connect, APSTA portal+scan (Task 4)
- `captive_dns.{h,c}` — UDP:53 DNS hijack task (Task 5)
- `portal_http.{h,c}` — HTTP server: form, save, captive-probe redirect (Task 6)
- `ui_setup.{h,c}` — LVGL setup card (Task 7)
- `net_manager.{h,c}` — boot state machine (Task 8)
- `main.c`, `data_task.c`, `firmware/main/CMakeLists.txt`, `secrets.h.example` — wiring (Task 9)
- **Deleted:** `wifi_sta.{h,c}` (Task 9)

---

## Task 1: Core — form-body parser

**Files:**
- Create: `core/include/nem/provision.h`
- Create: `core/src/provision.c`
- Create: `core/test/test_provision.c`
- Modify: `core/CMakeLists.txt` (add source + test)
- Modify: `firmware/components/nem_core/CMakeLists.txt` (add source)

**Interfaces:**
- Produces: `nem_prov_form_t` struct; `bool nem_provision_parse_form(const char *body, size_t len, nem_prov_form_t *out)` — URL-decodes an `application/x-www-form-urlencoded` body into `ssid`/`password`/`proxy_url`/`proxy_token`. Returns `true` iff a non-empty `ssid` was parsed **and** no field exceeded its cap.

- [ ] **Step 1: Write the header**

Create `core/include/nem/provision.h`:

```c
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
```

- [ ] **Step 2: Write the failing test**

Create `core/test/test_provision.c`:

```c
#include "unity.h"
#include "nem/provision.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_parse_happy(void) {
    const char *b = "ssid=MyNet&password=s3cret&proxy_url=http%3A%2F%2F1.2.3.4%3A8080%2Fnem&proxy_token=abc";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("MyNet", f.ssid);
    TEST_ASSERT_EQUAL_STRING("s3cret", f.password);
    TEST_ASSERT_EQUAL_STRING("http://1.2.3.4:8080/nem", f.proxy_url);
    TEST_ASSERT_EQUAL_STRING("abc", f.proxy_token);
}

static void test_parse_plus_is_space(void) {
    const char *b = "ssid=my+home+net&password=";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("my home net", f.ssid);
    TEST_ASSERT_EQUAL_STRING("", f.password);
}

static void test_parse_missing_ssid_fails(void) {
    const char *b = "password=x&proxy_url=y";
    nem_prov_form_t f;
    TEST_ASSERT_FALSE(nem_provision_parse_form(b, strlen(b), &f));
}

static void test_parse_empty_ssid_fails(void) {
    const char *b = "ssid=&password=x";
    nem_prov_form_t f;
    TEST_ASSERT_FALSE(nem_provision_parse_form(b, strlen(b), &f));
}

static void test_parse_overlong_ssid_fails(void) {
    /* 40 chars > NEM_PROV_SSID_MAX (32) */
    const char *b = "ssid=0123456789012345678901234567890123456789";
    nem_prov_form_t f;
    TEST_ASSERT_FALSE(nem_provision_parse_form(b, strlen(b), &f));
}

static void test_parse_ignores_unknown_and_last_wins(void) {
    const char *b = "junk=1&ssid=__other__&extra=2&ssid=Real";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("Real", f.ssid);   /* later key overwrites earlier */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_happy);
    RUN_TEST(test_parse_plus_is_space);
    RUN_TEST(test_parse_missing_ssid_fails);
    RUN_TEST(test_parse_empty_ssid_fails);
    RUN_TEST(test_parse_overlong_ssid_fails);
    RUN_TEST(test_parse_ignores_unknown_and_last_wins);
    return UNITY_END();
}
```

- [ ] **Step 3: Register the source and test in CMake**

In `core/CMakeLists.txt`, add `src/provision.c` to the `nem_core` source list (after `src/proxy_client.c`):

```cmake
  src/proxy_client.c
  src/provision.c
)
```

And add the test at the end of the file (after `nem_add_test(test_proxy_client)`):

```cmake
nem_add_test(test_provision)
```

In `firmware/components/nem_core/CMakeLists.txt`, add the source after `proxy_client.c`:

```cmake
        "${CORE}/src/proxy_client.c"
        "${CORE}/src/provision.c"
    INCLUDE_DIRS "${CORE}/include"
```

- [ ] **Step 4: Run test to verify it fails**

Run:
```bash
cmake -S core -B core/build >/dev/null && cmake --build core/build --target test_provision 2>&1 | tail -20
```
Expected: link/compile FAIL — `undefined reference to 'nem_provision_parse_form'` (implementation not written yet).

- [ ] **Step 5: Write the implementation**

Create `core/src/provision.c`:

```c
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
```

- [ ] **Step 6: Run test to verify it passes**

Run:
```bash
cmake --build core/build --target test_provision && ctest --test-dir core/build -R test_provision --output-on-failure
```
Expected: `test_provision` PASS (6 tests).

- [ ] **Step 7: Confirm the full core suite still passes**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build
```
Expected: `100% tests passed` (now 10 tests).

- [ ] **Step 8: Commit**

```bash
git add core/include/nem/provision.h core/src/provision.c core/test/test_provision.c core/CMakeLists.txt firmware/components/nem_core/CMakeLists.txt
git commit -m "feat(core): provisioning form-body parser (host-tested)"
```

---

## Task 2: Core — captive DNS reply builder

**Files:**
- Modify: `core/include/nem/provision.h` (add declaration)
- Modify: `core/src/provision.c` (add implementation)
- Modify: `core/test/test_provision.c` (add tests)

**Interfaces:**
- Consumes: nothing new.
- Produces: `int nem_provision_build_dns_reply(const unsigned char *query, int qlen, const unsigned char ip[4], unsigned char *out, int out_cap)` — copies the query, sets QR + ANCOUNT=1, appends a single A-record answer (name pointer `0xC00C`, TTL 60) pointing at `ip`. Returns reply length, or `-1` on malformed query / insufficient buffer.

- [ ] **Step 1: Add the declaration**

In `core/include/nem/provision.h`, replace the `/* (Task 2 adds ... here.) */` line with:

```c
/* Build a DNS response answering the single-question query in `query` with an
 * A record pointing at ip[4]. Writes into out[0..out_cap). Returns the reply
 * length, or -1 if the query is too short or the buffer is too small. */
int nem_provision_build_dns_reply(const unsigned char *query, int qlen,
                                  const unsigned char ip[4],
                                  unsigned char *out, int out_cap);
```

- [ ] **Step 2: Write the failing test**

Add these to `core/test/test_provision.c` (above `main`), and add their `RUN_TEST` lines to `main`:

```c
static void test_dns_reply_shape(void) {
    /* Query: header (ID=0x1234, RD set, QD=1) + question www.example.com A IN */
    unsigned char q[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        3, 'w','w','w', 7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0,
        0x00, 0x01, 0x00, 0x01
    };
    unsigned char ip[4] = { 192, 168, 4, 1 };
    unsigned char out[128];
    int n = nem_provision_build_dns_reply(q, (int)sizeof q, ip, out, (int)sizeof out);

    TEST_ASSERT_EQUAL_INT((int)sizeof q + 16, n);   /* query + 16-byte answer */
    TEST_ASSERT_TRUE(out[2] & 0x80);                /* QR (response) bit set  */
    TEST_ASSERT_EQUAL_UINT8(0x00, out[6]);          /* ANCOUNT hi             */
    TEST_ASSERT_EQUAL_UINT8(0x01, out[7]);          /* ANCOUNT lo = 1         */
    /* Answer begins right after the copied query. */
    unsigned char *a = out + sizeof q;
    TEST_ASSERT_EQUAL_UINT8(0xC0, a[0]);            /* name pointer           */
    TEST_ASSERT_EQUAL_UINT8(0x0C, a[1]);            /* -> offset 12           */
    TEST_ASSERT_EQUAL_UINT8(0x00, a[2]);            /* TYPE A hi              */
    TEST_ASSERT_EQUAL_UINT8(0x01, a[3]);            /* TYPE A lo              */
    /* Last 4 bytes are the IP. */
    TEST_ASSERT_EQUAL_UINT8(192, out[n - 4]);
    TEST_ASSERT_EQUAL_UINT8(168, out[n - 3]);
    TEST_ASSERT_EQUAL_UINT8(4,   out[n - 2]);
    TEST_ASSERT_EQUAL_UINT8(1,   out[n - 1]);
}

static void test_dns_reply_rejects_short_and_small_buf(void) {
    unsigned char ip[4] = { 192, 168, 4, 1 };
    unsigned char out[128];
    unsigned char tiny[8] = {0};
    TEST_ASSERT_EQUAL_INT(-1, nem_provision_build_dns_reply(tiny, 8, ip, out, sizeof out));

    unsigned char q[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        3, 'w','w','w', 0, 0x00, 0x01, 0x00, 0x01
    };
    TEST_ASSERT_EQUAL_INT(-1, nem_provision_build_dns_reply(q, (int)sizeof q, ip, out, 10));
}
```

Add to `main`:
```c
    RUN_TEST(test_dns_reply_shape);
    RUN_TEST(test_dns_reply_rejects_short_and_small_buf);
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```bash
cmake --build core/build --target test_provision 2>&1 | tail -20
```
Expected: FAIL — `undefined reference to 'nem_provision_build_dns_reply'`.

- [ ] **Step 4: Write the implementation**

Append to `core/src/provision.c`:

```c
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
```

- [ ] **Step 5: Run test to verify it passes**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build
```
Expected: `100% tests passed` (test_provision now 8 tests).

- [ ] **Step 6: Commit**

```bash
git add core/include/nem/provision.h core/src/provision.c core/test/test_provision.c
git commit -m "feat(core): captive DNS A-record reply builder (host-tested)"
```

---

## Task 3: Firmware — NVS credentials store (`net_creds`)

**Files:**
- Create: `firmware/main/net_creds.h`
- Create: `firmware/main/net_creds.c`
- Modify: `firmware/main/CMakeLists.txt` (add source)

**Interfaces:**
- Consumes: `nem_prov_form_t` (Task 1).
- Produces: `net_creds_t` (typedef of `nem_prov_form_t`); `bool net_creds_load(net_creds_t *out)` (true iff a non-empty ssid is available from NVS or the `secrets.h` seed); `bool net_creds_save(const net_creds_t *c)`.

- [ ] **Step 1: Write the header**

Create `firmware/main/net_creds.h`:

```c
#ifndef NET_CREDS_H
#define NET_CREDS_H

#include <stdbool.h>
#include "nem/provision.h"

/* Same shape as the parsed form: ssid/password/proxy_url/proxy_token. */
typedef nem_prov_form_t net_creds_t;

/* Load creds from NVS namespace "nem". If NVS has no ssid, seed from secrets.h
 * (if that header is present at build time). Returns true iff a non-empty ssid
 * is available. */
bool net_creds_load(net_creds_t *out);

/* Persist creds to NVS namespace "nem". Returns true on success. */
bool net_creds_save(const net_creds_t *c);

#endif
```

- [ ] **Step 2: Write the implementation**

Create `firmware/main/net_creds.c`:

```c
#include "net_creds.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

/* secrets.h is optional (gitignored). Include it only if present so a fresh
 * checkout without it still builds and goes through the portal. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#define NS "nem"

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap) {
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

bool net_creds_load(net_creds_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, "ssid", out->ssid,        sizeof out->ssid);
        load_str(h, "pass", out->password,    sizeof out->password);
        load_str(h, "url",  out->proxy_url,   sizeof out->proxy_url);
        load_str(h, "tok",  out->proxy_token, sizeof out->proxy_token);
        nvs_close(h);
    }
    if (out->ssid[0] == '\0') {
#ifdef NEM_WIFI_SSID
        strlcpy(out->ssid,     NEM_WIFI_SSID,     sizeof out->ssid);
        strlcpy(out->password, NEM_WIFI_PASSWORD, sizeof out->password);
#endif
#ifdef NEM_PROXY_URL
        if (out->proxy_url[0] == '\0')
            strlcpy(out->proxy_url, NEM_PROXY_URL, sizeof out->proxy_url);
#endif
    }
    return out->ssid[0] != '\0';
}

bool net_creds_save(const net_creds_t *c) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", c->ssid)        == ESP_OK
           && nvs_set_str(h, "pass", c->password)    == ESP_OK
           && nvs_set_str(h, "url",  c->proxy_url)   == ESP_OK
           && nvs_set_str(h, "tok",  c->proxy_token) == ESP_OK
           && nvs_commit(h)                          == ESP_OK;
    nvs_close(h);
    return ok;
}
```

- [ ] **Step 3: Register the source**

In `firmware/main/CMakeLists.txt`, add `net_creds.c` to `SRCS` (keep the rest for now):

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c" "net_fetch.c" "data_task.c" "net_creds.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls)
```

- [ ] **Step 4: Build to verify it compiles**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/net_creds.h firmware/main/net_creds.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): NVS credentials store with secrets.h seed"
```

---

## Task 4: Firmware — WiFi control (`wifi_ctrl`)

**Files:**
- Create: `firmware/main/wifi_ctrl.h`
- Create: `firmware/main/wifi_ctrl.c`
- Modify: `firmware/main/CMakeLists.txt` (add source)

**Interfaces:**
- Consumes: `net_creds_t` (Task 3).
- Produces:
  - `wifi_ctrl_ap_t` (`{ char ssid[33]; int8_t rssi; }`) and `WIFI_CTRL_SCAN_MAX` (16).
  - `esp_err_t wifi_ctrl_init(void)` — idempotent one-time NVS/netif/event/wifi init (no start).
  - `esp_err_t wifi_ctrl_sta_connect(const net_creds_t *c, int max_retries)` — STA mode, blocks until GOT_IP (ESP_OK) or `max_retries` disconnects (ESP_FAIL).
  - `esp_err_t wifi_ctrl_portal_start(const char *ap_ssid, const char *ap_pass, wifi_ctrl_ap_t *scan_out, int *scan_n)` — APSTA mode, scans (fills `scan_out`/`*scan_n`), starts WPA2 SoftAP at 192.168.4.1.

- [ ] **Step 1: Write the header**

Create `firmware/main/wifi_ctrl.h`:

```c
#ifndef WIFI_CTRL_H
#define WIFI_CTRL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "net_creds.h"

#define WIFI_CTRL_SCAN_MAX 16

typedef struct {
    char   ssid[33];
    int8_t rssi;
} wifi_ctrl_ap_t;

/* One-time init of nvs_flash, netif, default event loop, STA+AP netifs, and
 * esp_wifi. Does NOT start wifi. Idempotent. */
esp_err_t wifi_ctrl_init(void);

/* Connect as STA using creds; blocks until GOT_IP or `max_retries` disconnects.
 * Returns ESP_OK on success, ESP_FAIL otherwise. */
esp_err_t wifi_ctrl_sta_connect(const net_creds_t *c, int max_retries);

/* Bring up the provisioning radio: APSTA mode, scan nearby APs (fills scan_out /
 * *scan_n up to WIFI_CTRL_SCAN_MAX), and start WPA2 SoftAP `ap_ssid` at
 * 192.168.4.1. */
esp_err_t wifi_ctrl_portal_start(const char *ap_ssid, const char *ap_pass,
                                 wifi_ctrl_ap_t *scan_out, int *scan_n);

#endif
```

- [ ] **Step 2: Write the implementation**

Create `firmware/main/wifi_ctrl.c`:

```c
#include "wifi_ctrl.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
static int  s_retries, s_max_retries;
static bool s_inited;

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < s_max_retries) {
            s_retries++; esp_wifi_connect(); ESP_LOGW(TAG, "retry %d", s_retries);
        } else {
            xEventGroupSetBits(s_events, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t wifi_ctrl_init(void) {
    if (s_inited) return ESP_OK;
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL));
    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_ctrl_sta_connect(const net_creds_t *c, int max_retries) {
    s_retries = 0;
    s_max_retries = max_retries;
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid,     c->ssid,     sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, c->password, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", c->ssid);

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_ctrl_portal_start(const char *ap_ssid, const char *ap_pass,
                                 wifi_ctrl_ap_t *scan_out, int *scan_n) {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    *scan_n = 0;
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t num = WIFI_CTRL_SCAN_MAX;
        wifi_ap_record_t recs[WIFI_CTRL_SCAN_MAX];
        if (esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
            int n = 0;
            for (int i = 0; i < (int)num && n < WIFI_CTRL_SCAN_MAX; i++) {
                if (recs[i].ssid[0] == 0) continue;
                strlcpy(scan_out[n].ssid, (char *)recs[i].ssid, sizeof scan_out[n].ssid);
                scan_out[n].rssi = recs[i].rssi;
                n++;
            }
            *scan_n = n;
        }
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid,     ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap.ap.password, ap_pass, sizeof ap.ap.password);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGI(TAG, "SoftAP \"%s\" up at 192.168.4.1 (%d APs scanned)", ap_ssid, *scan_n);
    return ESP_OK;
}
```

- [ ] **Step 3: Register the source**

In `firmware/main/CMakeLists.txt`, add `wifi_ctrl.c` to `SRCS`:

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls)
```

- [ ] **Step 4: Build to verify it compiles**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/wifi_ctrl.h firmware/main/wifi_ctrl.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): wifi_ctrl — STA connect + APSTA portal/scan"
```

---

## Task 5: Firmware — captive DNS task (`captive_dns`)

**Files:**
- Create: `firmware/main/captive_dns.h`
- Create: `firmware/main/captive_dns.c`
- Modify: `firmware/main/CMakeLists.txt` (add source + `lwip` REQUIRES)

**Interfaces:**
- Consumes: `nem_provision_build_dns_reply` (Task 2).
- Produces: `void captive_dns_start(void)` — spawns a UDP:53 task answering every A query with 192.168.4.1.

- [ ] **Step 1: Write the header**

Create `firmware/main/captive_dns.h`:

```c
#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

/* Start a UDP:53 task that resolves every A query to 192.168.4.1 (captive
 * portal DNS hijack). Call after the SoftAP is up. */
void captive_dns_start(void);

#endif
```

- [ ] **Step 2: Write the implementation**

Create `firmware/main/captive_dns.c`:

```c
#include "captive_dns.h"
#include "nem/provision.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dns";

static void dns_task(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        ESP_LOGE(TAG, "socket/bind failed");
        if (s >= 0) close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS on :53");

    const unsigned char ip[4] = { 192, 168, 4, 1 };
    unsigned char q[512], r[600];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        int rl = nem_provision_build_dns_reply(q, n, ip, r, sizeof r);
        if (rl > 0) sendto(s, r, rl, 0, (struct sockaddr *)&from, fl);
    }
}

void captive_dns_start(void) {
    xTaskCreate(dns_task, "dns", 4096, NULL, 4, NULL);
}
```

- [ ] **Step 3: Register the source and lwip dependency**

In `firmware/main/CMakeLists.txt`, add `captive_dns.c` to `SRCS` and `lwip` to `REQUIRES`:

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip)
```

- [ ] **Step 4: Build to verify it compiles**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/captive_dns.h firmware/main/captive_dns.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): captive DNS hijack task"
```

---

## Task 6: Firmware — portal HTTP server (`portal_http`)

**Files:**
- Create: `firmware/main/portal_http.h`
- Create: `firmware/main/portal_http.c`
- Modify: `firmware/main/CMakeLists.txt` (add source + `esp_http_server` REQUIRES)

**Interfaces:**
- Consumes: `wifi_ctrl_ap_t` (Task 4), `net_creds_load`/`net_creds_save` (Task 3), `nem_provision_parse_form` (Task 1), `ui_setup_status` (Task 7 — declared in `ui_setup.h`, which Task 7 creates; this task references it, so build only fully links after Task 7 — acceptable since both land before the wiring task, but to keep THIS task's build green it includes a local forward declaration guard, see Step 2).
- Produces: `void portal_http_start(const wifi_ctrl_ap_t *aps, int ap_n)` — starts the HTTP server; a valid `POST /save` persists creds and reboots.

> Note: To keep each task's build green independently, this task does **not** call `ui_setup_status` yet (that on-screen "Saved…" update is wired in Task 9, once `ui_setup.h` exists). The save handler still fully persists + reboots.

- [ ] **Step 1: Write the header**

Create `firmware/main/portal_http.h`:

```c
#ifndef PORTAL_HTTP_H
#define PORTAL_HTTP_H

#include "wifi_ctrl.h"

/* Start the captive-portal HTTP server. `aps`/`ap_n` is the cached scan list
 * shown in the SSID dropdown (borrowed pointer; must outlive the server). A
 * valid POST /save persists creds to NVS and reboots. */
void portal_http_start(const wifi_ctrl_ap_t *aps, int ap_n);

#endif
```

- [ ] **Step 2: Write the implementation**

Create `firmware/main/portal_http.c`:

```c
#include "portal_http.h"
#include "net_creds.h"
#include "nem/provision.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "portal";
static const wifi_ctrl_ap_t *s_aps;
static int s_apn;

static esp_err_t send_form(httpd_req_t *req, const char *err) {
    net_creds_t cur;
    net_creds_load(&cur);   /* for prefill; ignore return */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>NEM Buddy setup</title><style>"
        "body{font-family:sans-serif;max-width:24rem;margin:2rem auto;padding:0 1rem}"
        "label{display:block;margin:.75rem 0 .2rem;font-weight:600}"
        "input,select{width:100%;padding:.5rem;font-size:1rem;box-sizing:border-box}"
        "button{margin-top:1.2rem;width:100%;padding:.7rem;font-size:1rem}"
        ".err{color:#b00;font-weight:600}</style><h2>NEM Buddy Wi-Fi setup</h2>");
    if (err) {
        httpd_resp_sendstr_chunk(req, "<p class=err>");
        httpd_resp_sendstr_chunk(req, err);
        httpd_resp_sendstr_chunk(req, "</p>");
    }
    httpd_resp_sendstr_chunk(req,
        "<form method=POST action=/save "
        "onsubmit=\"s.value=sel.value=='__other__'?oth.value:sel.value\">"
        "<input type=hidden name=ssid id=s>"
        "<label>Wi-Fi network</label>"
        "<select id=sel onchange=\"oth.style.display=this.value=='__other__'?'block':'none'\">");
    char row[128];
    for (int i = 0; i < s_apn; i++) {
        snprintf(row, sizeof row, "<option>%s</option>", s_aps[i].ssid);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req,
        "<option value=__other__>Other / hidden\xE2\x80\xA6</option></select>"
        "<input id=oth placeholder='hidden SSID' style='display:none'>"
        "<label>Password</label><input name=password type=password>"
        "<label>Proxy URL</label><input name=proxy_url value='");
    httpd_resp_sendstr_chunk(req, cur.proxy_url);
    httpd_resp_sendstr_chunk(req,
        "'><label>Proxy token (optional)</label><input name=proxy_token value='");
    httpd_resp_sendstr_chunk(req, cur.proxy_token);
    httpd_resp_sendstr_chunk(req, "'><button>Save &amp; connect</button></form>");
    httpd_resp_sendstr_chunk(req, NULL);   /* end chunks */
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req) {
    return send_form(req, NULL);
}

static esp_err_t save_post(httpd_req_t *req) {
    static char body[1024];
    int total = 0, r;
    while ((r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(body) - 1) break;
    }
    if (total < 0) return ESP_FAIL;
    body[total] = '\0';

    net_creds_t f;   /* net_creds_t == nem_prov_form_t */
    if (!nem_provision_parse_form(body, (size_t)total, &f))
        return send_form(req, "Please pick or enter a Wi-Fi network.");
    if (!net_creds_save(&f))
        return send_form(req, "Could not save — please try again.");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;text-align:center;margin-top:3rem'>"
        "<h2>Saved \xE2\x80\x94 connecting\xE2\x80\xA6</h2>"
        "<p>NEM Buddy will restart and join your network.</p>");
    ESP_LOGI(TAG, "creds saved; rebooting");
    vTaskDelay(pdMS_TO_TICKS(1200));   /* let the response flush */
    esp_restart();
    return ESP_OK;
}

/* Any unknown URL (OS captive-probe endpoints) -> redirect to the form. */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void portal_http_start(const wifi_ctrl_ap_t *aps, int ap_n) {
    s_aps = aps;
    s_apn = ap_n;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, redirect_404);
    ESP_LOGI(TAG, "portal HTTP server started");
}
```

- [ ] **Step 3: Register the source and dependency**

In `firmware/main/CMakeLists.txt`, add `portal_http.c` to `SRCS` and `esp_http_server` to `REQUIRES`:

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server)
```

- [ ] **Step 4: Build to verify it compiles**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/portal_http.h firmware/main/portal_http.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): captive-portal HTTP server (form + save + reboot)"
```

---

## Task 7: Firmware — setup card UI (`ui_setup`)

**Files:**
- Create: `firmware/main/ui_setup.h`
- Create: `firmware/main/ui_setup.c`
- Modify: `firmware/main/CMakeLists.txt` (add source)

**Interfaces:**
- Produces: `void ui_setup_show(const char *ap_ssid, const char *ap_pass, const char *portal_url)` and `void ui_setup_status(const char *msg)`. Both must be called inside `bsp_display_lock()/unlock()`.

- [ ] **Step 1: Write the header**

Create `firmware/main/ui_setup.h`:

```c
#ifndef UI_SETUP_H
#define UI_SETUP_H

/* Replace the active screen with a Wi-Fi setup card. Call inside
 * bsp_display_lock()/unlock(). */
void ui_setup_show(const char *ap_ssid, const char *ap_pass, const char *portal_url);

/* Update the status line on the setup card. Call inside the display lock. */
void ui_setup_status(const char *msg);

#endif
```

- [ ] **Step 2: Write the implementation**

Create `firmware/main/ui_setup.c`:

```c
#include "ui_setup.h"
#include "lvgl.h"

static lv_obj_t *s_status;

void ui_setup_show(const char *ap_ssid, const char *ap_pass, const char *portal_url) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 380, 380);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1b2027), 0);
    lv_obj_set_style_border_width(card, 0, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Wi-Fi setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text_fmt(body,
        "1. Join Wi-Fi:\n"
        "   %s\n"
        "   password: %s\n\n"
        "2. Open in a browser:\n"
        "   %s",
        ap_ssid, ap_pass, portal_url);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_18, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 4, 48);

    s_status = lv_label_create(card);
    lv_label_set_text(s_status, "Waiting for setup\xE2\x80\xA6");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void ui_setup_status(const char *msg) {
    if (s_status) lv_label_set_text(s_status, msg);
}
```

- [ ] **Step 3: Register the source**

In `firmware/main/CMakeLists.txt`, add `ui_setup.c` to `SRCS`:

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "ui_setup.c" "wifi_sta.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server)
```

- [ ] **Step 4: Build to verify it compiles**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/ui_setup.h firmware/main/ui_setup.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): LVGL Wi-Fi setup card"
```

---

## Task 8: Firmware — boot state machine (`net_manager`)

**Files:**
- Create: `firmware/main/net_manager.h`
- Create: `firmware/main/net_manager.c`
- Modify: `firmware/main/CMakeLists.txt` (add source)

**Interfaces:**
- Consumes: `wifi_ctrl_*` (Task 4), `net_creds_load` (Task 3), `captive_dns_start` (Task 5), `portal_http_start` (Task 6), `ui_setup_show`/`ui_setup_status` (Task 7), `data_task_start` (existing).
- Produces: `void net_manager_start(void)` — spawns the provisioning/connect state-machine task. Defines the default `NEM_SETUP_AP_PASSWORD`.

- [ ] **Step 1: Write the header**

Create `firmware/main/net_manager.h`:

```c
#ifndef NET_MANAGER_H
#define NET_MANAGER_H

/* Spawn the boot state machine: load creds -> STA connect, or fall back to the
 * SoftAP captive portal. On success it starts the data task. Call once from
 * app_main after the dashboard shell is created. */
void net_manager_start(void);

#endif
```

- [ ] **Step 2: Write the implementation**

Create `firmware/main/net_manager.c`:

```c
#include "net_manager.h"
#include "wifi_ctrl.h"
#include "net_creds.h"
#include "captive_dns.h"
#include "portal_http.h"
#include "ui_setup.h"
#include "data_task.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#ifndef NEM_SETUP_AP_PASSWORD
#define NEM_SETUP_AP_PASSWORD "nembuddy"   /* WPA2 needs >= 8 chars */
#endif
#define STA_MAX_RETRIES 5

static const char *TAG = "net";
static wifi_ctrl_ap_t s_aps[WIFI_CTRL_SCAN_MAX];

static void net_task(void *arg) {
    (void)arg;
    /* Let the first dashboard render settle before WiFi grabs internal DMA RAM. */
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_ERROR_CHECK(wifi_ctrl_init());

    net_creds_t creds;
    bool have = net_creds_load(&creds);
    if (have && wifi_ctrl_sta_connect(&creds, STA_MAX_RETRIES) == ESP_OK) {
        ESP_LOGI(TAG, "connected; starting data task");
        data_task_start();
        vTaskDelete(NULL);
        return;
    }

    /* PORTAL */
    ESP_LOGW(TAG, "no creds or connect failed; entering setup portal");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap[32];
    snprintf(ap, sizeof ap, "NEM-Buddy-%02X%02X", mac[4], mac[5]);

    bsp_display_lock(-1);
    ui_setup_show(ap, NEM_SETUP_AP_PASSWORD, "http://192.168.4.1");
    bsp_display_unlock();

    int n = 0;
    wifi_ctrl_portal_start(ap, NEM_SETUP_AP_PASSWORD, s_aps, &n);
    captive_dns_start();
    portal_http_start(s_aps, n);
    ESP_LOGI(TAG, "portal ready (%d APs); awaiting setup", n);
    vTaskDelete(NULL);   /* server + DNS tasks keep running; reboot on save */
}

void net_manager_start(void) {
    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 5, NULL, tskNO_AFFINITY);
}
```

- [ ] **Step 3: Register the source**

In `firmware/main/CMakeLists.txt`, add `net_manager.c` to `SRCS`:

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "ui_setup.c" "wifi_sta.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c" "net_manager.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server)
```

- [ ] **Step 4: Build to verify it compiles**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/net_manager.h firmware/main/net_manager.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): net_manager boot state machine"
```

---

## Task 9: Wire-up — switch app to net_manager, retire `wifi_sta`, read proxy from creds

**Files:**
- Modify: `firmware/main/main.c`
- Modify: `firmware/main/data_task.c`
- Modify: `firmware/main/portal_http.c` (add on-screen "Saved…" status)
- Modify: `firmware/main/CMakeLists.txt` (drop `wifi_sta.c`)
- Delete: `firmware/main/wifi_sta.c`, `firmware/main/wifi_sta.h`
- Modify: `firmware/main/secrets.h.example`

**Interfaces:**
- Consumes: everything from Tasks 3–8.
- Produces: final integrated boot flow. `data_task` now reads `proxy_url`/`proxy_token` from NVS creds and passes the token as the HTTP bearer.

- [ ] **Step 1: Point `main.c` at `net_manager`**

Replace `firmware/main/main.c` entirely with:

```c
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui_dashboard.h"
#include "net_manager.h"

static const char *TAG = "nem-buddy";

void app_main(void)
{
    ESP_LOGI(TAG, "NEM Buddy starting");
    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_brightness_set(80);

    bsp_display_lock(-1);
    ui_dashboard_create(lv_screen_active());
    bsp_display_unlock();

    net_manager_start();
}
```

- [ ] **Step 2: Make `data_task` read creds and pass the bearer token**

Replace `firmware/main/data_task.c` entirely with:

```c
#include "data_task.h"
#include "net_creds.h"
#include "net_fetch.h"
#include "ui_dashboard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "nem/proxy_client.h"
#include "nem/config.h"

static const char *TAG = "data";
#define PROXY_BUF_SZ (8 * 1024)

static void data_task(void *arg)
{
    (void)arg;
    net_creds_t creds;
    net_creds_load(&creds);
    if (creds.proxy_url[0] == '\0') {
        ESP_LOGW(TAG, "no proxy configured; data task idle");
        vTaskDelete(NULL);
        return;
    }
    const char *bearer = creds.proxy_token[0] ? creds.proxy_token : NULL;

    nem_config_t cfg; nem_config_defaults(&cfg);
    char *buf = heap_caps_malloc(PROXY_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no PSRAM buffer"); vTaskDelete(NULL); return; }

    for (;;) {
        int len = 0;
        if (nem_http_get(creds.proxy_url, bearer, buf, PROXY_BUF_SZ, &len) == ESP_OK) {
            nem_snapshot_t snap;
            nem_region_mix_t mix;
            if (nem_proxy_parse(buf, &snap, &mix)) {
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                const nem_fuel_mix_t *hm = &mix.regions[cfg.home_region];
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, hm, cfg.home_region);
                bsp_display_unlock();
                ESP_LOGI(TAG, "ok: %s $%.1f  demand %.0f  ren %.0f%%",
                         nem_region_name(cfg.home_region), h->price, h->demand_mw,
                         hm->renewable_fraction * 100.0);
            } else {
                ESP_LOGW(TAG, "proxy parse failed (%d bytes)", len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

void data_task_start(void)
{
    xTaskCreatePinnedToCore(data_task, "data", 8192, NULL, 5, NULL, tskNO_AFFINITY);
}
```

- [ ] **Step 3: Show the on-screen "Saved…" status from the portal**

In `firmware/main/portal_http.c`, add the include near the top (after `#include "nem/provision.h"`):

```c
#include "ui_setup.h"
#include "bsp/esp-bsp.h"
```

Then in `save_post`, immediately after the `httpd_resp_sendstr(req, ...)` "Saved — connecting…" response and before `vTaskDelay`, add:

```c
    bsp_display_lock(-1);
    ui_setup_status("Saved \xE2\x80\x94 connecting\xE2\x80\xA6");
    bsp_display_unlock();
```

- [ ] **Step 4: Remove `wifi_sta` from the build and delete it**

Replace the `SRCS` line in `firmware/main/CMakeLists.txt` so `wifi_sta.c` is gone (final form):

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "ui_setup.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c" "net_manager.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server)
```

Then delete the files:
```bash
rm firmware/main/wifi_sta.c firmware/main/wifi_sta.h
```

- [ ] **Step 5: Update `secrets.h.example` to document the seed + AP override**

Replace `firmware/main/secrets.h.example` with:

```c
#ifndef SECRETS_H
#define SECRETS_H
/* OPTIONAL first-boot seed. Copy to secrets.h (gitignored) to skip the setup
 * portal on a fresh flash (dev convenience). If absent, the device boots into
 * the SoftAP captive portal instead. NVS is always the source of truth once
 * provisioned. */
#define NEM_WIFI_SSID     "your-wifi-ssid"
#define NEM_WIFI_PASSWORD "your-wifi-password"
#define NEM_PROXY_URL     "http://192.168.1.50:8080/nem"
/* Optional: override the WPA2 password of the setup AP (>= 8 chars).
   #define NEM_SETUP_AP_PASSWORD "nembuddy" */
#endif
```

- [ ] **Step 6: Build the final integrated firmware**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -20
```
Expected: `Project build complete`. No reference to `wifi_sta`.

- [ ] **Step 7: Confirm the core suite is still green**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build
```
Expected: `100% tests passed` (10 tests).

- [ ] **Step 8: Commit**

```bash
git add firmware/main/main.c firmware/main/data_task.c firmware/main/portal_http.c firmware/main/CMakeLists.txt firmware/main/secrets.h.example
git rm firmware/main/wifi_sta.c firmware/main/wifi_sta.h
git commit -m "feat(firmware): boot via net_manager; proxy from NVS creds; retire wifi_sta"
```

---

## Task 10: On-board UAT (human-flashed)

**Files:** none (verification only).

This task is performed by the human operator flashing the board. The agent prepares by (a) confirming the build is clean and (b) temporarily removing/renaming `firmware/main/secrets.h` so scenarios 1–4 exercise the true no-seed path.

**Interfaces:** Consumes the full integrated firmware from Task 9.

- [ ] **Step 1: Ensure a no-seed build for the portal scenarios**

If `firmware/main/secrets.h` exists, rename it so the seed path is inactive:
```bash
[ -f firmware/main/secrets.h ] && mv firmware/main/secrets.h firmware/main/secrets.h.bak || echo "no secrets.h present"
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -5
```

- [ ] **Step 2: Human flashes and runs the UAT checklist**

Ask the human to flash and confirm each scenario (from the design spec):

Run (human):
```bash
source ~/esp/idf-env.sh && idf.py -C firmware -p /dev/cu.usbmodem21101 flash monitor
```

Confirm:
1. Fresh flash, NVS empty, no `secrets.h` → boots to the setup card showing `NEM-Buddy-XXXX` + the WPA2 password; joining with it auto-pops the captive sheet; the SSID dropdown lists real nearby networks.
2. Submit good creds (+ proxy URL) → "Saved — connecting…" → device reboots → dashboard shows live data.
3. Submit a wrong password → reboot → STA fails after 5 retries → returns to the setup card automatically.
4. Power-cycle after a successful setup → boots straight to the dashboard (NVS persisted).
5. Restore `secrets.h` (`mv firmware/main/secrets.h.bak firmware/main/secrets.h`), rebuild+flash with NVS erased (`idf.py -C firmware erase-flash flash`) → seeds from `secrets.h` and connects with no portal.
6. Throughout, the AMOLED renders cleanly during AP-up and STA-up (no white flush / `ESP_ERR_NO_MEM`).

- [ ] **Step 3: Record results**

Note any failures against the scenario number. If all six pass, Plan 3b is complete. Update project memory (`project-status.md`) to mark Plan 3b done and Plan 4 next.

---

## Self-Review

**Spec coverage:**
- Decision 1 (self-healing trigger, 5 retries) → Task 8 (`STA_MAX_RETRIES 5`, portal fallback). ✅
- Decision 2 (true captive portal) → Tasks 5 (DNS) + 6 (HTTP redirect on 404). ✅
- Decision 3 (scan-and-pick SSID) → Task 4 (`wifi_ctrl_portal_start` scan) + Task 6 (dropdown + "other"). ✅
- Decision 4 (proxy URL + optional token, prefilled) → Task 6 (prefill from `net_creds_load`) + Task 9 (bearer passthrough). ✅
- Decision 5 (`secrets.h` seed) → Task 3 (`__has_include` + `#ifdef` seed) + Task 9 (`secrets.h.example`). ✅
- Decision 6 (reboot-after-save) → Task 6 (`esp_restart` in `save_post`). ✅
- Decision 7 (WPA2 AP, fixed pw shown on card) → Task 4 (`WIFI_AUTH_WPA2_PSK`) + Task 7 (card shows pw) + Task 8 (`NEM_SETUP_AP_PASSWORD`). ✅
- Host-tested parse + DNS builder → Tasks 1–2. ✅
- Error handling (empty scan, NVS fail, blank proxy) → Task 6 (`send_form` error re-render), Task 9 (`data_task` idle on blank proxy). ✅
- UAT scenarios 1–6 → Task 10. ✅
- Out-of-scope items (internet deployment, TLS spike) correctly excluded. ✅

**Placeholder scan:** No TBD/TODO; every code step is complete. ✅

**Type consistency:** `net_creds_t` is a typedef of `nem_prov_form_t`, so `nem_provision_parse_form(..., &f)` in Task 6 feeds `net_creds_save(&f)` with no conversion. `wifi_ctrl_ap_t`, `WIFI_CTRL_SCAN_MAX`, `net_creds_load/save`, `captive_dns_start`, `portal_http_start`, `ui_setup_show/status`, `net_manager_start`, `nem_provision_build_dns_reply` signatures match across their definition and call sites. ✅
