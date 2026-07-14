# Multi-Client Derived-Key Auth — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let many operator-provisioned NEM Buddy devices read the shared plain-HTTP proxy, each with its own derived key, by replacing Plan 5's single shared secret + request counter with per-device keys derived from one master secret and a stateless request authenticator.

**Architecture:** The proxy stores only a master secret and derives each device's key on the fly from the `X-NEM-Id` header (`device_key = HMAC(SHA256(master), device_id)`). Requests carry a constant-message HMAC (no counter, no clock); responses are HMAC-signed per device. Data integrity still rests on the signed response plus the on-device `settlement_epoch` freshness gate. The proxy keeps no per-device state.

**Tech Stack:** Python 3 stdlib (proxy), C11 + Unity (host-tested core), ESP-IDF v5.5 + mbedTLS (firmware), Docker + Kubernetes + cloudflared (deploy).

**Spec:** `docs/superpowers/specs/2026-07-14-multi-client-derived-key-auth-design.md`

**Branch:** `feat/proxy-auth` (revises the Plan 5 auth layer in place; not yet merged).

## Global Constraints

- **Key hierarchy (verbatim):** `master_key = SHA256(master_secret_utf8)` (32 bytes, proxy-only). `device_key = HMAC-SHA256(master_key, device_id_utf8)` (32 bytes, provisioned onto the device). The device holds `device_id` + `device_key`; never the master.
- **Wire protocol:** request headers `X-NEM-Id: <device_id>`, `X-NEM-Auth: base64(HMAC-SHA256(device_key, "GET /nem"))`, `Accept-Encoding: identity`. Response headers `X-NEM-Sig: base64(HMAC-SHA256(device_key, body_bytes))` + `Cache-Control: no-store, no-transform`. Base64 is standard (with `=` padding). The request-MAC message is the **constant** `"GET /nem"` — no counter.
- **LAN mode:** proxy with no `NEM_PROXY_SECRET` ⇒ no request check, no `X-NEM-Sig`. Device with empty `device_id`/`device_key` ⇒ send no auth headers, don't verify. Device with a key set but response `X-NEM-Sig` absent/wrong ⇒ **fail closed** (reject).
- **Revocation:** `NEM_PROXY_DENY` = comma-separated `device_id`s; the proxy 401s a listed id before verifying.
- **Freshness (kept from Plan 5):** device accepts a payload iff `settlement_epoch >= last_accepted_epoch` (reject strictly older).
- **Core C build:** `-Wall -Wextra -Werror`, C11, Unity. Build+run: `cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build --output-on-failure`.
- **Firmware build:** `source ~/esp/idf-env.sh && idf.py -C firmware build`. Flash (human, board on USB): `idf.py -C firmware -p /dev/cu.usbmodem21101 flash`. Never run `flash monitor` in the background; capture serial with the pyserial reset-and-read script.
- **Known-answer test vector (used across languages — copy exactly):**
  - master_secret = `testmaster`
  - master_key (hex) = `d9095f2c47b0a651748521077659d03539f4376f17ed79ef070c32d2f6198fb1`
  - device_id = `dev01`
  - device_key (hex) = `8f87ad65dbbf2716af81b89332efbd009d67a64a72499ce5b204df787c41484c`
  - device_key (base64) = `j4etZdu/JxavgbiTMu+9AJ1npkpySZzlsgTfeHxBSEw=`
  - message `"GET /nem"` → `X-NEM-Auth` = `AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=`
  - body `{"t":"2026-07-14T00:00:00","regions":[]}` → `X-NEM-Sig` = `uPGokXyiUFAQFM1FjIK8dW3EGa3Rgm23GDpxFIc+VGk=`
  - second device `device_id = dev02` → device_key (hex) = `90eafea9d5492e7ef1d3201c6453c612a70f33d98786bb029f642dc1f6e712a3` (for wrong-device / revoke tests)
  - RFC 4231 case 2 (mbedTLS sanity): HMAC-SHA256(key=`Jefe`, `what do ya want for nothing?`) = `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`

---

## Task 1: Core proxy_auth — constant request message, drop the counter

Remove the counter-reservation and counter-message helpers (dead once the request MAC is a constant); replace the request message with a compile-time constant; keep the freshness helper.

**Files:**
- Modify: `core/include/nem/proxy_auth.h`
- Modify: `core/src/proxy_auth.c`
- Modify: `core/test/test_proxy_auth.c`

**Interfaces:**
- Produces:
  - `#define NEM_AUTH_REQ_MSG "GET /nem"` — the canonical request-MAC message.
  - `bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch);` — unchanged (true iff `new_epoch >= last_epoch`).
- Removes: `nem_auth_req_message`, `nem_auth_reserve` (callers are updated in Task 5).

- [ ] **Step 1: Rewrite the test**

Replace `core/test/test_proxy_auth.c` with:

```c
#include "unity.h"
#include "nem/proxy_auth.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_req_msg_constant(void) {
    TEST_ASSERT_EQUAL_STRING("GET /nem", NEM_AUTH_REQ_MSG);
}

static void test_accept_fresh_rules(void) {
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 0));    /* first ever */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(200, 100));  /* newer  */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 100));  /* same == accept */
    TEST_ASSERT_FALSE(nem_auth_accept_fresh(99, 100));  /* strictly older = replay */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_req_msg_constant);
    RUN_TEST(test_accept_fresh_rules);
    return UNITY_END();
}
```

- [ ] **Step 2: Rewrite the header**

Replace `core/include/nem/proxy_auth.h` with:

```c
#ifndef NEM_PROXY_AUTH_H
#define NEM_PROXY_AUTH_H

#include <stdbool.h>

/* Canonical request-MAC message (constant). The request MAC proves possession of
 * the device key without transmitting it; no counter, no clock. */
#define NEM_AUTH_REQ_MSG "GET /nem"

/* Freshness: accept a payload iff its settlement epoch is not strictly older. */
bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch);

#endif
```

- [ ] **Step 3: Rewrite the implementation**

Replace `core/src/proxy_auth.c` with:

```c
#include "nem/proxy_auth.h"

bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch) {
    return new_epoch >= last_epoch;
}
```

- [ ] **Step 4: Build + run the test**

Run:
```bash
cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build -R test_proxy_auth --output-on-failure
```
Expected: `test_proxy_auth ... Passed`, `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add core/include/nem/proxy_auth.h core/src/proxy_auth.c core/test/test_proxy_auth.c
git commit -m "refactor(core): constant request-MAC message; drop counter helpers"
```

---

## Task 2: Core provision — add a device_id form field

The captive portal now collects a device id. Add it to the parsed provisioning form so both firmware and portal can carry it.

**Files:**
- Modify: `core/include/nem/provision.h`
- Modify: `core/src/provision.c`
- Modify: `core/test/test_provision.c`

**Interfaces:**
- Produces: `nem_prov_form_t` gains `char device_id[NEM_PROV_DEVID_MAX + 1];`; the form parser recognises the `device_id` key.

- [ ] **Step 1: Extend the happy-path test**

In `core/test/test_provision.c`, replace `test_parse_happy` with the version below (adds `device_id` to the body + an assertion):

```c
static void test_parse_happy(void) {
    const char *b = "ssid=MyNet&password=s3cret&proxy_url=http%3A%2F%2F1.2.3.4%3A8080%2Fnem&proxy_token=abc&device_id=dev01";
    nem_prov_form_t f;
    TEST_ASSERT_TRUE(nem_provision_parse_form(b, strlen(b), &f));
    TEST_ASSERT_EQUAL_STRING("MyNet", f.ssid);
    TEST_ASSERT_EQUAL_STRING("s3cret", f.password);
    TEST_ASSERT_EQUAL_STRING("http://1.2.3.4:8080/nem", f.proxy_url);
    TEST_ASSERT_EQUAL_STRING("abc", f.proxy_token);
    TEST_ASSERT_EQUAL_STRING("dev01", f.device_id);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cmake --build core/build 2>&1 | tail -5
```
Expected: compile error — `nem_prov_form_t` has no member named `device_id`.

- [ ] **Step 3: Add the struct field + max**

In `core/include/nem/provision.h`, add the max alongside the others:

```c
#define NEM_PROV_TOKEN_MAX  128
#define NEM_PROV_DEVID_MAX  64
```

Add the field to the struct (after `proxy_token`):

```c
    char proxy_token[NEM_PROV_TOKEN_MAX + 1];
    char device_id[NEM_PROV_DEVID_MAX + 1];
```

Update the parser doc comment's key list to read: `Recognises keys ssid, password, proxy_url, proxy_token, device_id;`.

- [ ] **Step 4: Register the field in the parser**

In `core/src/provision.c`, add the entry to the `fields[]` table (after the `proxy_token` row):

```c
        { "proxy_token", out->proxy_token, NEM_PROV_TOKEN_MAX },
        { "device_id",   out->device_id,   NEM_PROV_DEVID_MAX },
```

- [ ] **Step 5: Build + run the test**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build -R test_provision --output-on-failure
```
Expected: `test_provision ... Passed`.

- [ ] **Step 6: Commit**

```bash
git add core/include/nem/provision.h core/src/provision.c core/test/test_provision.c
git commit -m "feat(core): parse device_id provisioning field"
```

---

## Task 3: Proxy — per-device derived keys, device-id verify/sign, denylist

Replace the single-key auth with per-device derived keys keyed on `X-NEM-Id`, add a denylist, and drop the request counter.

**Files:**
- Modify: `proxy/nem_proxy.py`
- Modify: `proxy/test_auth.py`

**Interfaces:**
- Produces (module-level in `nem_proxy.py`):
  - `derive_master_key(secret: str) -> bytes` — `sha256(secret.encode())`.
  - `derive_device_key(master_key: bytes, device_id: str) -> bytes` — `HMAC-SHA256(master_key, device_id)`.
  - `sign_body(key: bytes, body: bytes) -> str` — `base64(HMAC-SHA256(key, body))`.
  - `verify_request(device_key: bytes, auth_b64: str) -> bool` — constant-time check of the request MAC over `"GET /nem"`.

- [ ] **Step 1: Rewrite the test**

Replace `proxy/test_auth.py` with:

```python
import base64, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import nem_proxy as p

# Known-answer vector (see plan Global Constraints)
MK = p.derive_master_key("testmaster")
assert MK.hex() == "d9095f2c47b0a651748521077659d03539f4376f17ed79ef070c32d2f6198fb1"
DK = p.derive_device_key(MK, "dev01")
assert DK.hex() == "8f87ad65dbbf2716af81b89332efbd009d67a64a72499ce5b204df787c41484c"
DK2 = p.derive_device_key(MK, "dev02")

def test_sign_body():
    body = b'{"t":"2026-07-14T00:00:00","regions":[]}'
    assert p.sign_body(DK, body) == "uPGokXyiUFAQFM1FjIK8dW3EGa3Rgm23GDpxFIc+VGk="

def test_verify_request_ok():
    assert p.verify_request(DK, "AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=") is True

def test_verify_request_bad_mac():
    assert p.verify_request(DK, "AAAA") is False

def test_verify_request_wrong_device():
    # dev01's auth presented against dev02's key -> mismatch
    assert p.verify_request(DK2, "AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=") is False

if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn(); print("ok", name)
    print("ALL PASS")
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
python3 proxy/test_auth.py
```
Expected: `AttributeError: module 'nem_proxy' has no attribute 'derive_master_key'`.

- [ ] **Step 3: Replace the auth config + functions**

In `proxy/nem_proxy.py`, replace this block (added in Plan 5):

```python
_secret = os.environ.get("NEM_PROXY_SECRET", "")
_auth_key = None            # bytes, or None in LAN mode
_last_ctr = [-1]            # highest accepted request counter (guarded by _lock)


def derive_key(secret):
    return hashlib.sha256(secret.encode()).digest()


def sign_body(key, body):
    return base64.b64encode(hmac.new(key, body, hashlib.sha256).digest()).decode()


def verify_request(key, ctr, auth_b64):
    msg = ("GET /nem\n%d" % ctr).encode()
    expect = base64.b64encode(hmac.new(key, msg, hashlib.sha256).digest()).decode()
    return hmac.compare_digest(expect, auth_b64 or "")
```

with:

```python
_secret = os.environ.get("NEM_PROXY_SECRET", "")
_master_key = None          # bytes, or None in LAN mode
_deny = {d.strip() for d in os.environ.get("NEM_PROXY_DENY", "").split(",") if d.strip()}


def derive_master_key(secret):
    return hashlib.sha256(secret.encode()).digest()


def derive_device_key(master_key, device_id):
    return hmac.new(master_key, device_id.encode(), hashlib.sha256).digest()


def sign_body(key, body):
    return base64.b64encode(hmac.new(key, body, hashlib.sha256).digest()).decode()


def verify_request(device_key, auth_b64):
    expect = base64.b64encode(hmac.new(device_key, b"GET /nem", hashlib.sha256).digest()).decode()
    return hmac.compare_digest(expect, auth_b64 or "")
```

- [ ] **Step 4: Rewrite `do_GET`**

Replace the current `Handler.do_GET` body with:

```python
    def do_GET(self):
        device_key = None
        if _master_key is not None:
            did = self.headers.get("X-NEM-Id", "")
            if not did or did in _deny:
                self.send_response(401); self.end_headers(); return
            device_key = derive_device_key(_master_key, did)
            auth = self.headers.get("X-NEM-Auth", "")
            if not verify_request(device_key, auth):
                self.send_response(401); self.end_headers(); return
        with _lock:
            payload = _cache["payload"]
        if payload is None:
            self.send_response(503)
            self.end_headers()
            self.wfile.write(b'{"error":"warming up"}')
            return
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if _master_key is not None:
            self.send_header("X-NEM-Sig", sign_body(device_key, body))
            self.send_header("Cache-Control", "no-store, no-transform")
        self.end_headers()
        self.wfile.write(body)
```

- [ ] **Step 5: Update the `main()` init**

In `main()`, replace the Plan 5 init block:

```python
    global _auth_key
    if _secret:
        _auth_key = derive_key(_secret)
        print("[proxy] app-layer auth ENABLED", file=sys.stderr)
    else:
        print("[proxy] app-layer auth disabled (LAN mode)", file=sys.stderr)
```

with:

```python
    global _master_key
    if _secret:
        _master_key = derive_master_key(_secret)
        print("[proxy] app-layer auth ENABLED (%d device(s) denied)" % len(_deny), file=sys.stderr)
    else:
        print("[proxy] app-layer auth disabled (LAN mode)", file=sys.stderr)
```

- [ ] **Step 6: Run the unit test to verify it passes**

Run:
```bash
python3 proxy/test_auth.py
```
Expected: `ok test_*` lines then `ALL PASS`.

- [ ] **Step 7: Local integration check (auth + revoke)**

Run (uses live AEMO, no OE key needed; wait for warm-up):
```bash
AUTH=$(python3 - <<'PY'
import hashlib,hmac,base64
mk=hashlib.sha256(b"testmaster").digest()
dk=hmac.new(mk,b"dev01",hashlib.sha256).digest()
print(base64.b64encode(hmac.new(dk,b"GET /nem",hashlib.sha256).digest()).decode())
PY
)
NEM_PROXY_SECRET=testmaster python3 proxy/nem_proxy.py --port 8099 >/tmp/px.log 2>&1 &
PX=$!; sleep 20
echo "--- no X-NEM-Id -> 401 ---"
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8099/nem
echo "--- valid dev01 -> 200 + X-NEM-Sig ---"
curl -s -D - -o /dev/null "http://127.0.0.1:8099/nem" -H "X-NEM-Id: dev01" -H "X-NEM-Auth: $AUTH" | grep -iE "HTTP/|X-NEM-Sig"
kill $PX 2>/dev/null; sleep 1
echo "--- revoke dev01 -> 401 ---"
NEM_PROXY_SECRET=testmaster NEM_PROXY_DENY=dev01 python3 proxy/nem_proxy.py --port 8099 >/tmp/px.log 2>&1 &
PX=$!; sleep 20
curl -s -o /dev/null -w "%{http_code}\n" "http://127.0.0.1:8099/nem" -H "X-NEM-Id: dev01" -H "X-NEM-Auth: $AUTH"
kill $PX 2>/dev/null
```
Expected: `401`; then `HTTP/1.0 200 OK` + an `X-NEM-Sig:` header; then `401` (revoked).

- [ ] **Step 8: Commit**

```bash
git add proxy/nem_proxy.py proxy/test_auth.py
git commit -m "feat(proxy): per-device derived keys + device-id auth + denylist"
```

---

## Task 4: Proxy — provisioning helper + deploy docs

A helper to derive a device credential from the master secret, plus RUNBOOK/manifest updates for the multi-client model.

**Files:**
- Create: `proxy/provision_device.py`
- Modify: `deploy/RUNBOOK.md`
- Modify: `deploy/k8s/nem-proxy.yaml`

- [ ] **Step 1: Write the helper**

Create `proxy/provision_device.py`:

```python
#!/usr/bin/env python3
"""Derive a device_id + device_key for the NEM Buddy proxy.

Usage:
  NEM_PROXY_SECRET=<master> python3 proxy/provision_device.py <device_id>

Prints the device_id and its base64 device_key to enter in the device captive
portal. The master secret never leaves this machine.
"""
import base64, hashlib, hmac, os, sys


def main():
    if len(sys.argv) != 2:
        print("usage: NEM_PROXY_SECRET=<master> python3 provision_device.py <device_id>",
              file=sys.stderr)
        sys.exit(2)
    secret = os.environ.get("NEM_PROXY_SECRET", "")
    if not secret:
        print("error: NEM_PROXY_SECRET not set", file=sys.stderr)
        sys.exit(2)
    device_id = sys.argv[1]
    master_key = hashlib.sha256(secret.encode()).digest()
    device_key = hmac.new(master_key, device_id.encode(), hashlib.sha256).digest()
    print("device_id:  " + device_id)
    print("device_key: " + base64.b64encode(device_key).decode())


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify it against the known-answer vector**

Run:
```bash
NEM_PROXY_SECRET=testmaster python3 proxy/provision_device.py dev01
```
Expected exactly:
```
device_id:  dev01
device_key: j4etZdu/JxavgbiTMu+9AJ1npkpySZzlsgTfeHxBSEw=
```

- [ ] **Step 3: Update the k8s manifest comment**

In `deploy/k8s/nem-proxy.yaml`, replace:

```yaml
  replicas: 1
  strategy: { type: Recreate }        # never two pods with divergent in-memory state
```

with:

```yaml
  replicas: 1                         # auth is stateless; this cap is an OE-quota choice
  strategy: { type: Recreate }        # one in-memory payload cache per pod
```

- [ ] **Step 4: Update the RUNBOOK**

In `deploy/RUNBOOK.md`, replace the §1 body:

```markdown
## 1. Secrets
    kubectl create secret generic nem-proxy-secrets \
      --from-literal=oe-api-key='oe_xxx' \
      --from-literal=proxy-secret='<long-random-passphrase>'
The same `proxy-secret` value is what you enter in the device's captive-portal
"Proxy token" field.
```

with:

```markdown
## 1. Secrets
    kubectl create secret generic nem-proxy-secrets \
      --from-literal=oe-api-key='oe_xxx' \
      --from-literal=proxy-secret='<long-random-master-passphrase>'
`proxy-secret` is the **master** secret. Devices never hold it — each device gets its
own derived key (see §7). To revoke devices, set `NEM_PROXY_DENY` (comma-separated
device IDs) on the deployment:
    kubectl set env deploy/nem-proxy NEM_PROXY_DENY=alice-01,bob-03
```

Replace the §6 verify block:

```markdown
## 6. Verify through Cloudflare
    curl -s -o /dev/null -w "%{http_code}\n" http://nembuddy.<domain>/nem   # 401 (no auth)
    # 200 with valid headers (compute X-NEM-Auth with the proxy-secret + a counter)
Confirm no 301→https redirect and that `X-NEM-Sig` is present on the 200.
```

with:

```markdown
## 6. Verify through Cloudflare
    curl -s -o /dev/null -w "%{http_code}\n" http://nembuddy.<domain>/nem   # 401 (no X-NEM-Id)
    # 200 with valid headers for a provisioned device:
    AUTH=$(python3 - <<'PY'
    import hashlib,hmac,base64
    mk=hashlib.sha256(b"<master-passphrase>").digest()
    dk=hmac.new(mk,b"<device_id>",hashlib.sha256).digest()
    print(base64.b64encode(hmac.new(dk,b"GET /nem",hashlib.sha256).digest()).decode())
    PY
    )
    curl -s -D - -o /dev/null "http://nembuddy.<domain>/nem" \
      -H "X-NEM-Id: <device_id>" -H "X-NEM-Auth: $AUTH" | grep -iE "HTTP/|X-NEM-Sig|location"
Confirm no 301→https redirect and that `X-NEM-Sig` is present on the 200.
```

Replace the §7 device block:

```markdown
## 7. Device
Re-provision via the captive portal: proxy URL = `http://nembuddy.<domain>/nem`,
proxy token = the `proxy-secret`.
```

with:

```markdown
## 7. Device provisioning (per device)
Derive each device's credential from the master secret:
    NEM_PROXY_SECRET='<master-passphrase>' python3 proxy/provision_device.py rjk-kitchen
This prints a `device_id` and a base64 `device_key`. In the device captive portal set:
proxy URL = `http://nembuddy.<domain>/nem`, Device ID = `rjk-kitchen`, Device key = the
printed base64 value. Use a unique ID per device.
```

- [ ] **Step 5: Commit**

```bash
git add proxy/provision_device.py deploy/RUNBOOK.md deploy/k8s/nem-proxy.yaml
git commit -m "feat(deploy): device provisioning helper + multi-client runbook"
```

---

## Task 5: Firmware — device-id request auth, drop the counter

Swap the firmware auth path to send `X-NEM-Id` + a constant-message MAC keyed by a ready-made device key, and delete the NVS request counter.

**Files:**
- Modify: `firmware/main/net_fetch.h`
- Modify: `firmware/main/net_fetch.c`
- Modify: `firmware/main/data_task.c`
- Delete: `firmware/main/auth_counter.c`, `firmware/main/auth_counter.h`
- Modify: `firmware/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `NEM_AUTH_REQ_MSG` (Task 1); `creds.device_id` (Task 2); `creds.proxy_token` now holds the base64 device key.
- Produces: `typedef struct { const uint8_t *key; const char *device_id; } nem_auth_t;` (`key` is 32 bytes or NULL for LAN mode).

- [ ] **Step 1: Update the `nem_auth_t` struct in the header**

In `firmware/main/net_fetch.h`, replace:

```c
/* key = 32-byte SHA256(token), or key=NULL for LAN mode (no auth, no verify). */
typedef struct {
    const uint8_t     *key;
    unsigned long long counter;
} nem_auth_t;
```

with:

```c
/* key = 32-byte device key, or key=NULL for LAN mode (no auth, no verify).
 * device_id is sent as X-NEM-Id; NULL/empty means LAN mode. */
typedef struct {
    const uint8_t *key;
    const char    *device_id;
} nem_auth_t;
```

- [ ] **Step 2: Update the request-signing block in `net_fetch.c`**

In `firmware/main/net_fetch.c`, replace:

```c
    bool secured = auth && auth->key;
    if (secured) {
        char msg[40], b64[45], ctr[24];
        int mlen = nem_auth_req_message(msg, sizeof msg, auth->counter);
        hmac_b64(auth->key, (const uint8_t *)msg, (size_t)mlen, b64);
        snprintf(ctr, sizeof ctr, "%llu", auth->counter);
        esp_http_client_set_header(c, "X-NEM-Ctr", ctr);
        esp_http_client_set_header(c, "X-NEM-Auth", b64);
        esp_http_client_set_header(c, "Accept-Encoding", "identity");
    }
```

with:

```c
    bool secured = auth && auth->key && auth->device_id && auth->device_id[0];
    if (secured) {
        char b64[45];
        hmac_b64(auth->key, (const uint8_t *)NEM_AUTH_REQ_MSG, strlen(NEM_AUTH_REQ_MSG), b64);
        esp_http_client_set_header(c, "X-NEM-Id", auth->device_id);
        esp_http_client_set_header(c, "X-NEM-Auth", b64);
        esp_http_client_set_header(c, "Accept-Encoding", "identity");
    }
```

- [ ] **Step 3: Update the self-test in `net_fetch.c`**

Replace the whole `nem_http_auth_selftest` function with:

```c
bool nem_http_auth_selftest(void)
{
    uint8_t mk[32], dk[32];
    mbedtls_sha256((const unsigned char *)"testmaster", 10, mk, 0);      /* master_key */
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), mk, 32,
                    (const uint8_t *)"dev01", 5, dk);                    /* device_key */
    char mac[45], sig[45];
    hmac_b64(dk, (const uint8_t *)NEM_AUTH_REQ_MSG, strlen(NEM_AUTH_REQ_MSG), mac);
    const char *body = "{\"t\":\"2026-07-14T00:00:00\",\"regions\":[]}";
    hmac_b64(dk, (const uint8_t *)body, strlen(body), sig);
    bool ok = ct_eq(mac, "AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=")
           && ct_eq(sig, "uPGokXyiUFAQFM1FjIK8dW3EGa3Rgm23GDpxFIc+VGk=");
    ESP_LOGI(TAG, "auth self-test: %s", ok ? "PASS" : "FAIL");
    return ok;
}
```

- [ ] **Step 4: Update the auth setup in `data_task.c`**

In `firmware/main/data_task.c`, replace the include line:

```c
#include "auth_counter.h"
#include "mbedtls/sha256.h"
#include "nem/proxy_auth.h"
#include <string.h>
```

with (drop `auth_counter.h` + `sha256.h`; add `base64.h`):

```c
#include "mbedtls/base64.h"
#include "nem/proxy_auth.h"
#include <string.h>
```

Replace the key-derivation block:

```c
    uint8_t auth_key[32];
    bool secured = creds.proxy_token[0] != '\0';
    if (secured)
        mbedtls_sha256((const unsigned char *)creds.proxy_token, strlen(creds.proxy_token), auth_key, 0);
    nem_http_auth_selftest();
```

with (base64-decode the provisioned device key; secured only if id + a 32-byte key):

```c
    uint8_t auth_key[32];
    size_t klen = 0;
    bool secured = creds.device_id[0] != '\0' && creds.proxy_token[0] != '\0'
        && mbedtls_base64_decode(auth_key, sizeof auth_key, &klen,
               (const unsigned char *)creds.proxy_token, strlen(creds.proxy_token)) == 0
        && klen == 32;
    nem_http_auth_selftest();
```

Replace the per-iteration auth struct:

```c
        nem_auth_t auth = { .key = secured ? auth_key : NULL, .counter = auth_counter_next() };
```

with:

```c
        nem_auth_t auth = { .key = secured ? auth_key : NULL, .device_id = secured ? creds.device_id : NULL };
```

- [ ] **Step 5: Delete the counter module + deregister it**

Run:
```bash
git rm firmware/main/auth_counter.c firmware/main/auth_counter.h
```

In `firmware/main/CMakeLists.txt`, remove `"auth_counter.c"` from the `SRCS` list (leave `mbedtls` in `REQUIRES` — still used by `net_fetch.c`/`data_task.c`). The line becomes:

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "ui_setup.c" "ui_drill.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c" "net_manager.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server mbedtls)
```

- [ ] **Step 6: Build**

Run:
```bash
source ~/esp/idf-env.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -iE "error|Built target __idf_main|Project build complete" | grep -v kconfig
```
Expected: `Built target __idf_main` + `Project build complete`, no `error`.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/net_fetch.h firmware/main/net_fetch.c firmware/main/data_task.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): device-id request auth via derived key; remove NVS counter"
```

---

## Task 6: Firmware — persist + collect device_id (creds + portal)

Wire the device id through NVS and the captive portal so provisioning actually populates `creds.device_id` and stores the base64 device key.

**Files:**
- Modify: `firmware/main/net_creds.c`
- Modify: `firmware/main/portal_http.c`

- [ ] **Step 1: Load/save device_id in `net_creds.c`**

In `firmware/main/net_creds.c`, add the load line after the `tok` load:

```c
        load_str(h, "tok",  out->proxy_token, sizeof out->proxy_token);
        load_str(h, "did",  out->device_id,   sizeof out->device_id);
```

Add the save clause to the `&&` chain (after the `tok` set):

```c
           && nvs_set_str(h, "tok",  c->proxy_token) == ESP_OK
           && nvs_set_str(h, "did",  c->device_id)   == ESP_OK
           && nvs_commit(h)                          == ESP_OK;
```

- [ ] **Step 2: Add the Device ID field + relabel the key in `portal_http.c`**

In `firmware/main/portal_http.c`, replace:

```c
    html_escape(cur.proxy_token, ebuf, sizeof ebuf);
    snprintf(field, sizeof field,
             "<label>Proxy token (optional)</label>"
             "<input name=proxy_token value='%s'>", ebuf);
    httpd_resp_sendstr_chunk(req, field);
```

with:

```c
    html_escape(cur.device_id, ebuf, sizeof ebuf);
    snprintf(field, sizeof field,
             "<label>Device ID (optional)</label>"
             "<input name=device_id value='%s'>", ebuf);
    httpd_resp_sendstr_chunk(req, field);
    html_escape(cur.proxy_token, ebuf, sizeof ebuf);
    snprintf(field, sizeof field,
             "<label>Device key (base64, optional)</label>"
             "<input name=proxy_token value='%s'>", ebuf);
    httpd_resp_sendstr_chunk(req, field);
```

- [ ] **Step 3: Build**

Run:
```bash
source ~/esp/idf-env.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -iE "error|Built target __idf_main|Project build complete" | grep -v kconfig
```
Expected: `Built target __idf_main` + `Project build complete`, no `error`.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/net_creds.c firmware/main/portal_http.c
git commit -m "feat(firmware): persist + collect device_id in creds and captive portal"
```

---

## Task 7: End-to-end verification (human-driven UAT)

Hardware + cluster + Cloudflare in the loop. The agent produces the commands; **the user flashes the board(s), applies the deploy, and sets the Cloudflare toggles.**

**Files:** none (verification only).

- [ ] **Step 1: Deploy the authenticated proxy**

Follow `deploy/RUNBOOK.md` §1–§5: create the secret (real OE key + a master `proxy-secret`), push the image, `kubectl apply`, add the cloudflared ingress, and set the Cloudflare toggles (Always Use HTTPS OFF, etc.).

- [ ] **Step 2: Verify the public endpoint through Cloudflare**

Run RUNBOOK §6 for a chosen `<device_id>` (e.g. derive with `provision_device.py`). Expected: `401` with no `X-NEM-Id` (and no 301→https / no `location:`); then `200` with an `X-NEM-Sig` header for the valid request.

- [ ] **Step 3: Provision device #1 and confirm on-device**

Derive a credential: `NEM_PROXY_SECRET='<master>' python3 proxy/provision_device.py kitchen`. Flash the firmware (`idf.py -C firmware -p /dev/cu.usbmodem21101 flash`), then re-provision via the captive portal: URL `http://nembuddy.<domain>/nem`, Device ID `kitchen`, Device key = the printed base64. Capture serial:
```bash
source ~/esp/idf-env.sh >/dev/null 2>&1 && python3 - <<'PY'
import serial,time
s=serial.Serial("/dev/cu.usbmodem21101",115200,timeout=0.2)
s.setDTR(False); s.setRTS(True); time.sleep(0.12); s.setRTS(False)
buf=b""; t0=time.time()
while time.time()-t0<20: buf+=s.read(4096)
s.close()
for l in buf.decode("utf-8","replace").splitlines():
    if any(k in l for k in ("auth self-test","fetch:","ok:","signature","stale")): print(l[:160])
PY
```
Expected: `auth self-test: PASS`; `fetch: GET http://nembuddy... -> 200 ... (auth)`; `ok: ...`. No `signature INVALID`.

- [ ] **Step 4: Second device + revoke (multi-client proof)**

Provision a second id (`... provision_device.py study`) onto a second board (or re-provision the same board with the new id) and confirm it also updates. Then revoke the first: `kubectl set env deploy/nem-proxy NEM_PROXY_DENY=kitchen`. Re-capture serial on device #1 → its fetches should now 401 (dashboard stops updating) while device `study` keeps working. Restore with `kubectl set env deploy/nem-proxy NEM_PROXY_DENY-`.

- [ ] **Step 5: Tamper test (negative)**

Break the signer so the device fails closed: `kubectl set env deploy/nem-proxy NEM_PROXY_SECRET=wrongmaster` (proxy now derives different device keys than the device holds). Re-capture serial: expect `response signature INVALID — rejecting` and the dashboard stops updating. Restore the correct master.

- [ ] **Step 6: LAN regression (negative)**

Run the proxy locally with **no** `NEM_PROXY_SECRET`, provision the device with an **empty** Device ID + Device key + the LAN URL, and confirm `fetch: ... -> 200` (no `(auth)` suffix) and `ok: ...`.

- [ ] **Step 7: Update project memory + finish the branch**

Note in memory that the multi-client derived-key auth is implemented + UAT'd on `feat/proxy-auth` (proxy holds only a master secret; devices carry `device_id` + derived `device_key`; revoke via `NEM_PROXY_DENY`). Then merge `feat/proxy-auth` → `main` per the finishing-a-development-branch flow.

---

## Self-Review

**Spec coverage:** key hierarchy → Task 3 (proxy derive) + Task 5 (firmware ready-key) + Task 4 (helper); wire protocol v2 → Task 1 (constant), Task 3 (proxy headers), Task 5 (firmware headers); do_GET + denylist → Task 3; provisioning helper → Task 4; captive portal + creds device_id → Tasks 2 (core field) + 6 (persist/collect); firmware auth path + counter removal → Task 5; freshness gate kept → Task 5 (unchanged, still consumed in `data_task`); LAN mode → Tasks 3 + 5; revocation → Task 3 (+ RUNBOOK Task 4); deployment (replicas comment + RUNBOOK deny/provision) → Task 4; testing → per-task + Task 7 E2E. All covered.

**Placeholder scan:** none. `<domain>`, `<master>`, `<master-passphrase>`, `<device_id>`, `<registry>` in the RUNBOOK/Task 7 are genuine user-supplied values, not code placeholders. All code blocks are complete.

**Type consistency:** proxy `derive_master_key`/`derive_device_key`/`sign_body`/`verify_request(device_key, auth_b64)` are defined in Task 3 and used consistently in the Task 3 test and Task 4 helper (which mirrors the same math). `nem_auth_t{key, device_id}` and `NEM_AUTH_REQ_MSG` are defined in Tasks 5/1 and consumed in `net_fetch.c` + `data_task.c` (Task 5). `nem_prov_form_t.device_id` / `NEM_PROV_DEVID_MAX` defined in Task 2 and consumed in Tasks 5 (`creds.device_id`) and 6 (portal `name=device_id`, NVS `did`). `nem_auth_accept_fresh` unchanged (Task 1) and still called in `data_task` (untouched freshness gate). Known-answer vector values are identical across Tasks 1/3/4/5 and the Global Constraints.
```
