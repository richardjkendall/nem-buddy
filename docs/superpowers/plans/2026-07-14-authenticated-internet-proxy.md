# Authenticated Internet-Reachable Proxy — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the TLS-incapable ESP32 device read its data proxy over the public internet by authenticating the plain-HTTP link at the application layer (HMAC-SHA256), so a MITM can't feed it fake data and randoms can't burn the OpenElectricity quota.

**Architecture:** The existing Python proxy runs on the user's k8s cluster behind a Cloudflare Argo tunnel serving **plain HTTP** ("Always Use HTTPS" off). The device signs each request with an HMAC over a monotonic counter; the proxy signs each response body with an HMAC; both verify. Key = `SHA256(shared token)`, where the token is the existing provisioning field. No secret configured on either end ⇒ **LAN mode** (today's behaviour, no auth). Pure protocol logic lives host-tested in `core/`; crypto uses Python `hmac`/`hashlib` on the proxy and mbedTLS on the device.

**Tech Stack:** Python 3 stdlib (proxy), C11 + Unity (host-tested core), ESP-IDF v5.5 + mbedTLS (firmware), Docker + Kubernetes + cloudflared (deploy).

**Spec:** `docs/superpowers/specs/2026-07-14-nem-buddy-authenticated-internet-proxy-design.md`

## Global Constraints

- **Protocol (verbatim):** key = `SHA256(token_utf8)` (32 bytes). Request MAC message = the literal `"GET /nem\n" + <counter-decimal>`. Request headers: `X-NEM-Ctr: <decimal counter>`, `X-NEM-Auth: base64(HMAC-SHA256(key, message))`, `Accept-Encoding: identity`. Response header: `X-NEM-Sig: base64(HMAC-SHA256(key, body_bytes))` plus `Cache-Control: no-store, no-transform`. Base64 is standard (with `=` padding). Counter reservation gap = `1024`. Freshness: accept a payload iff its `settlement_epoch >= last_accepted_epoch` (reject strictly older).
- **LAN mode:** proxy with no `NEM_PROXY_SECRET` env ⇒ no request check, no `X-NEM-Sig`. Device with empty token ⇒ send no auth headers, don't verify. Device with a token set but response `X-NEM-Sig` absent/wrong ⇒ **fail closed** (reject).
- **One device per secret** (proxy tracks a single `last_seen` counter).
- **Core C build:** `-Wall -Wextra -Werror`, C11, Unity. Build+run: `cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build --output-on-failure`.
- **Firmware build:** `source ~/esp/idf-env.sh && idf.py -C firmware build`. Flash (human, board on USB): `idf.py -C firmware -p /dev/cu.usbmodem21101 flash`. Never run `flash monitor` in the background; capture serial with the pyserial reset-and-read script.
- **Known-answer test vector** (used across languages — copy exactly):
  - token = `testsecret`
  - key (hex) = `59953998e54a579be74c1b7344cd55c64981451b066a35c9d7baf5497f16d865`
  - message `GET /nem\n42` → `X-NEM-Auth` = `G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4=`
  - body `{"t":"2026-07-14T00:00:00","regions":[]}` → `X-NEM-Sig` = `iYW1w5bFInoap2OfScY0Xt082rEedKXacCGM4EZu11g=`
  - RFC 4231 case 2 (mbedTLS sanity): HMAC-SHA256(key=`Jefe`, `what do ya want for nothing?`) = `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`

---

## Task 1: Core protocol helpers (host-tested, no crypto)

Pure, portable pieces both ends agree on: the request-MAC message string, the freshness rule, and the counter-reservation arithmetic. Crypto (HMAC/SHA/base64) is done by mbedTLS on-device and Python on the proxy — not here.

**Files:**
- Create: `core/include/nem/proxy_auth.h`
- Create: `core/src/proxy_auth.c`
- Create: `core/test/test_proxy_auth.c`
- Modify: `core/CMakeLists.txt` (add source to `nem_core`; register `test_proxy_auth`)

**Interfaces:**
- Produces:
  - `int  nem_auth_req_message(char *out, size_t out_sz, unsigned long long counter);` — writes `"GET /nem\n<counter>"`, returns the length written (like snprintf).
  - `bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch);` — true iff `new_epoch >= last_epoch`.
  - `unsigned long long nem_auth_reserve(unsigned long long stored_floor, unsigned long long gap, unsigned long long *out_new_floor);` — returns the RAM counter start (`stored_floor`) and sets `*out_new_floor = stored_floor + gap`.

- [ ] **Step 1: Write the failing test**

Create `core/test/test_proxy_auth.c`:

```c
#include "unity.h"
#include "nem/proxy_auth.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_req_message_format(void) {
    char buf[32];
    int n = nem_auth_req_message(buf, sizeof buf, 42ULL);
    TEST_ASSERT_EQUAL_STRING("GET /nem\n42", buf);
    TEST_ASSERT_EQUAL_INT(11, n);   /* strlen("GET /nem\n42") */
}

static void test_req_message_large_counter(void) {
    char buf[40];
    nem_auth_req_message(buf, sizeof buf, 4294968320ULL);   /* > 32-bit */
    TEST_ASSERT_EQUAL_STRING("GET /nem\n4294968320", buf);
}

static void test_accept_fresh_rules(void) {
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 0));    /* first ever */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(200, 100));  /* newer  */
    TEST_ASSERT_TRUE(nem_auth_accept_fresh(100, 100));  /* same == no-op, accept */
    TEST_ASSERT_FALSE(nem_auth_accept_fresh(99, 100));  /* strictly older = replay */
}

static void test_reserve(void) {
    unsigned long long new_floor = 0;
    unsigned long long start = nem_auth_reserve(1000ULL, 1024ULL, &new_floor);
    TEST_ASSERT_EQUAL_UINT64(1000ULL, start);
    TEST_ASSERT_EQUAL_UINT64(2024ULL, new_floor);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_req_message_format);
    RUN_TEST(test_req_message_large_counter);
    RUN_TEST(test_accept_fresh_rules);
    RUN_TEST(test_reserve);
    return UNITY_END();
}
```

Create the header `core/include/nem/proxy_auth.h`:

```c
#ifndef NEM_PROXY_AUTH_H
#define NEM_PROXY_AUTH_H

#include <stdbool.h>
#include <stddef.h>

/* Canonical request-MAC message: "GET /nem\n<counter>". Returns snprintf length. */
int nem_auth_req_message(char *out, size_t out_sz, unsigned long long counter);

/* Freshness: accept a payload iff its settlement epoch is not strictly older. */
bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch);

/* Counter reservation: RAM counter starts at stored_floor; the new NVS floor to
 * persist is stored_floor + gap (guarantees monotonic counters across reboots). */
unsigned long long nem_auth_reserve(unsigned long long stored_floor,
                                    unsigned long long gap,
                                    unsigned long long *out_new_floor);

#endif
```

- [ ] **Step 2: Register the source + test in CMake**

In `core/CMakeLists.txt`, add `src/proxy_auth.c` to the `nem_core` source list (after `src/provision.c`):

```cmake
  src/provision.c
  src/proxy_auth.c
```

And add, after the last `nem_add_test(test_provision)`:

```cmake
nem_add_test(test_proxy_auth)
```

- [ ] **Step 3: Run the test to verify it fails (no implementation yet)**

Run:
```bash
cmake -S core -B core/build && cmake --build core/build 2>&1 | tail -5
```
Expected: build FAILS — `undefined reference to 'nem_auth_req_message'` (and the other two).

- [ ] **Step 4: Write the implementation**

Create `core/src/proxy_auth.c`:

```c
#include "nem/proxy_auth.h"
#include <stdio.h>

int nem_auth_req_message(char *out, size_t out_sz, unsigned long long counter) {
    return snprintf(out, out_sz, "GET /nem\n%llu", counter);
}

bool nem_auth_accept_fresh(long long new_epoch, long long last_epoch) {
    return new_epoch >= last_epoch;
}

unsigned long long nem_auth_reserve(unsigned long long stored_floor,
                                    unsigned long long gap,
                                    unsigned long long *out_new_floor) {
    *out_new_floor = stored_floor + gap;
    return stored_floor;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build -R test_proxy_auth --output-on-failure
```
Expected: `test_proxy_auth ... Passed`, `100% tests passed`.

- [ ] **Step 6: Commit**

```bash
git add core/include/nem/proxy_auth.h core/src/proxy_auth.c core/test/test_proxy_auth.c core/CMakeLists.txt
git commit -m "feat(core): proxy-auth protocol helpers (req message, freshness, counter reserve)"
```

---

## Task 2: Proxy — request auth, response signing, LAN mode

Add HMAC verification + signing to `proxy/nem_proxy.py`, gated on `NEM_PROXY_SECRET`. Testable functions plus a standalone test using the known-answer vector.

**Files:**
- Modify: `proxy/nem_proxy.py`
- Create: `proxy/test_auth.py`

**Interfaces:**
- Produces (module-level in `nem_proxy.py`):
  - `sign_body(key: bytes, body: bytes) -> str` — returns `base64(HMAC-SHA256(key, body))`.
  - `verify_request(key: bytes, ctr: int, auth_b64: str) -> bool` — constant-time check of the request MAC over `"GET /nem\n<ctr>"`.
  - `derive_key(secret: str) -> bytes` — `sha256(secret.encode())`.

- [ ] **Step 1: Write the failing test**

Create `proxy/test_auth.py`:

```python
import base64, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import nem_proxy as p

# Known-answer vector (see plan Global Constraints)
KEY = p.derive_key("testsecret")
assert KEY.hex() == "59953998e54a579be74c1b7344cd55c64981451b066a35c9d7baf5497f16d865"

def test_sign_body():
    body = b'{"t":"2026-07-14T00:00:00","regions":[]}'
    assert p.sign_body(KEY, body) == "iYW1w5bFInoap2OfScY0Xt082rEedKXacCGM4EZu11g="

def test_verify_request_ok():
    assert p.verify_request(KEY, 42, "G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4=") is True

def test_verify_request_bad_mac():
    assert p.verify_request(KEY, 42, "AAAA") is False

def test_verify_request_wrong_counter():
    # right MAC but for counter 42, presented as counter 43 -> mismatch
    assert p.verify_request(KEY, 43, "G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4=") is False

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
Expected: `AttributeError: module 'nem_proxy' has no attribute 'derive_key'`.

- [ ] **Step 3: Add the auth functions + config to `nem_proxy.py`**

Add `import base64, hashlib, hmac` to the imports block (top of the file, alongside the existing `import json`, `import os`, etc.).

Add these module-level definitions (place them just after the `_lock = threading.Lock()` line near the top):

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

- [ ] **Step 4: Enforce in the request handler + sign the response**

Replace the body of `Handler.do_GET` (currently serving the payload) with the version below. It rejects bad/replayed requests when auth is enabled and adds `X-NEM-Sig` + cache headers.

```python
    def do_GET(self):
        if _auth_key is not None:
            try:
                ctr = int(self.headers.get("X-NEM-Ctr", ""))
            except ValueError:
                self.send_response(401); self.end_headers(); return
            auth = self.headers.get("X-NEM-Auth", "")
            if not verify_request(_auth_key, ctr, auth):
                self.send_response(401); self.end_headers(); return
            with _lock:
                if ctr <= _last_ctr[0]:
                    self.send_response(401); self.end_headers(); return
                _last_ctr[0] = ctr
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
        if _auth_key is not None:
            self.send_header("X-NEM-Sig", sign_body(_auth_key, body))
            self.send_header("Cache-Control", "no-store, no-transform")
        self.end_headers()
        self.wfile.write(body)
```

- [ ] **Step 5: Initialise the key in `main()`**

In `main()`, after `api_key = os.environ.get("NEM_OE_API_KEY", "")`, add:

```python
    global _auth_key
    if _secret:
        _auth_key = derive_key(_secret)
        print("[proxy] app-layer auth ENABLED", file=sys.stderr)
    else:
        print("[proxy] app-layer auth disabled (LAN mode)", file=sys.stderr)
```

- [ ] **Step 6: Run the unit test to verify it passes**

Run:
```bash
python3 proxy/test_auth.py
```
Expected: `ok test_*` lines then `ALL PASS`.

- [ ] **Step 7: Local integration check (server + curl)**

Run (uses live AEMO, needs no OE key; wait for warm-up):
```bash
NEM_PROXY_SECRET=testsecret python3 proxy/nem_proxy.py --port 8099 >/tmp/px.log 2>&1 &
PX=$!; sleep 8
echo "--- no auth headers -> 401 ---"
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8099/nem
echo "--- valid vector (ctr 42) -> 200 + X-NEM-Sig ---"
curl -s -D - -o /dev/null "http://127.0.0.1:8099/nem" \
  -H "X-NEM-Ctr: 42" -H "X-NEM-Auth: G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4=" | grep -iE "HTTP/|X-NEM-Sig"
echo "--- replay ctr 42 again -> 401 ---"
curl -s -o /dev/null -w "%{http_code}\n" "http://127.0.0.1:8099/nem" \
  -H "X-NEM-Ctr: 42" -H "X-NEM-Auth: G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4="
kill $PX
```
Expected: first `401`; second shows `HTTP/1.0 200 OK` and an `X-NEM-Sig:` header; third `401` (counter 42 no longer > last-seen 42).

- [ ] **Step 8: Commit**

```bash
git add proxy/nem_proxy.py proxy/test_auth.py
git commit -m "feat(proxy): HMAC request auth + response signing (LAN-mode fallback)"
```

---

## Task 3: Container image

Package the proxy so k8s can run it. Verify locally with Docker before it touches the cluster.

**Files:**
- Create: `proxy/Dockerfile`
- Create: `proxy/.dockerignore`

- [ ] **Step 1: Write the Dockerfile**

Create `proxy/Dockerfile`:

```dockerfile
FROM python:3.12-slim
WORKDIR /app
COPY nem_proxy.py /app/nem_proxy.py
# stdlib only — no pip install needed
EXPOSE 8080
ENTRYPOINT ["python3", "/app/nem_proxy.py", "--port", "8080", "--host", "0.0.0.0"]
```

Create `proxy/.dockerignore`:

```
test_auth.py
__pycache__
*.pyc
```

- [ ] **Step 2: Build the image**

Run:
```bash
docker build -t nem-proxy:dev proxy
```
Expected: `Successfully tagged nem-proxy:dev` (or `naming to ... nem-proxy:dev done`).

- [ ] **Step 3: Run the container and verify auth end-to-end**

Run:
```bash
docker run -d --name nemproxy -p 8098:8080 -e NEM_PROXY_SECRET=testsecret nem-proxy:dev
sleep 8
echo "--- no auth -> 401 ---"; curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8098/nem
echo "--- valid vector -> 200 ---"; curl -s -o /dev/null -w "%{http_code}\n" \
  http://127.0.0.1:8098/nem -H "X-NEM-Ctr: 100" -H "X-NEM-Auth: $(python3 - <<'PY'
import hashlib,hmac,base64
k=hashlib.sha256(b"testsecret").digest()
print(base64.b64encode(hmac.new(k,b"GET /nem\n100",hashlib.sha256).digest()).decode())
PY
)"
docker rm -f nemproxy
```
Expected: `401` then `200`.

- [ ] **Step 4: Commit**

```bash
git add proxy/Dockerfile proxy/.dockerignore
git commit -m "feat(proxy): Dockerfile for containerised deployment"
```

---

## Task 4: Kubernetes manifests + Cloudflare runbook (user-applied)

Deployment artefacts and a runbook. The `kubectl apply` / Cloudflare toggles / image push are **the user's to run against their cluster** — this task delivers the files and validates them offline.

**Files:**
- Create: `deploy/k8s/nem-proxy.yaml`
- Create: `deploy/RUNBOOK.md`

- [ ] **Step 1: Write the k8s manifest**

Create `deploy/k8s/nem-proxy.yaml` (single replica — the intraday history + counter live in pod memory):

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: nem-proxy
  labels: { app: nem-proxy }
spec:
  replicas: 1
  strategy: { type: Recreate }        # never two pods with divergent in-memory state
  selector: { matchLabels: { app: nem-proxy } }
  template:
    metadata: { labels: { app: nem-proxy } }
    spec:
      containers:
        - name: nem-proxy
          image: nem-proxy:dev          # replace with your pushed image ref
          ports: [{ containerPort: 8080 }]
          env:
            - name: NEM_OE_API_KEY
              valueFrom: { secretKeyRef: { name: nem-proxy-secrets, key: oe-api-key } }
            - name: NEM_PROXY_SECRET
              valueFrom: { secretKeyRef: { name: nem-proxy-secrets, key: proxy-secret } }
          readinessProbe:
            httpGet: { path: /nem, port: 8080 }
            initialDelaySeconds: 10
            periodSeconds: 15
          resources:
            requests: { cpu: 25m, memory: 64Mi }
            limits:   { memory: 128Mi }
---
apiVersion: v1
kind: Service
metadata:
  name: nem-proxy
spec:
  selector: { app: nem-proxy }
  ports: [{ port: 8080, targetPort: 8080 }]
```

> Note: the readiness probe hits `/nem` without auth headers → returns 401 once auth is enabled, which still proves the pod is *serving*. If your k8s treats 401 as not-ready, change the probe to a `tcpSocket: { port: 8080 }` check.

- [ ] **Step 2: Write the runbook**

Create `deploy/RUNBOOK.md`:

```markdown
# NEM Proxy — deploy to k8s behind Cloudflare tunnel

## 1. Secrets
    kubectl create secret generic nem-proxy-secrets \
      --from-literal=oe-api-key='oe_xxx' \
      --from-literal=proxy-secret='<long-random-passphrase>'
The same `proxy-secret` value is what you enter in the device's captive-portal
"Proxy token" field.

## 2. Image
Build/push to a registry your cluster can pull, and set `image:` in
`nem-proxy.yaml` to that ref:
    docker build -t <registry>/nem-proxy:1 proxy && docker push <registry>/nem-proxy:1

## 3. Deploy
    kubectl apply -f deploy/k8s/nem-proxy.yaml

## 4. cloudflared tunnel ingress
Add to the tunnel config (or a `cloudflare.com` Tunnel → Public Hostname):
    - hostname: nembuddy.<your-domain>
      service: http://nem-proxy.<namespace>.svc.cluster.local:8080
    - service: http_status:404

## 5. Cloudflare zone settings for nembuddy.<your-domain>  (CRITICAL — device is HTTP-only)
- SSL/TLS → Edge Certificates → **Always Use HTTPS: OFF**.
- No HSTS header; no Page/Configuration Rule forcing HTTPS on this hostname.
- Speed → Optimization: minify OFF; Rocket Loader OFF (must not alter the body).
- Cache: the proxy already sends `Cache-Control: no-store, no-transform`.
- (Optional) Security → add a rate-limiting rule on the hostname.

## 6. Verify through Cloudflare
    curl -s -o /dev/null -w "%{http_code}\n" http://nembuddy.<domain>/nem   # 401 (no auth)
    # 200 with valid headers (compute X-NEM-Auth with the proxy-secret + a counter)
Confirm no 301→https redirect and that `X-NEM-Sig` is present on the 200.

## 7. Device
Re-provision via the captive portal: proxy URL = `http://nembuddy.<domain>/nem`,
proxy token = the `proxy-secret`.
```

- [ ] **Step 3: Validate the manifest offline**

Run:
```bash
python3 -c "import yaml,sys; list(yaml.safe_load_all(open('deploy/k8s/nem-proxy.yaml'))); print('yaml ok')" 2>/dev/null \
  || echo "(pyyaml not installed — skip; kubectl apply --dry-run=client -f deploy/k8s/nem-proxy.yaml on the cluster instead)"
```
Expected: `yaml ok` (or the skip note). The authoritative check is the user's `kubectl apply --dry-run=client`.

- [ ] **Step 4: Commit**

```bash
git add deploy/k8s/nem-proxy.yaml deploy/RUNBOOK.md
git commit -m "feat(deploy): k8s manifest + Cloudflare tunnel runbook"
```

---

## Task 5: Firmware — persistent request counter (NVS reservation)

A monotonic-across-reboot counter with bounded NVS writes, using the core reservation helper.

**Files:**
- Create: `firmware/main/auth_counter.h`
- Create: `firmware/main/auth_counter.c`
- Modify: `firmware/main/CMakeLists.txt` (add source + `mbedtls`/`nem_core` requires)

**Interfaces:**
- Produces: `unsigned long long auth_counter_next(void);` — returns a strictly increasing counter, never repeating across reboots.

- [ ] **Step 1: Write the header**

Create `firmware/main/auth_counter.h`:

```c
#ifndef AUTH_COUNTER_H
#define AUTH_COUNTER_H
/* Monotonic request counter, persisted in NVS with a reservation gap so it
 * survives reboots (always jumps forward) with ~1 NVS write per 1024 calls. */
unsigned long long auth_counter_next(void);
#endif
```

- [ ] **Step 2: Write the implementation**

Create `firmware/main/auth_counter.c`:

```c
#include "auth_counter.h"
#include "nem/proxy_auth.h"
#include "nvs.h"

#define NS  "nem"
#define KEY "actr"          /* u64: smallest counter not yet reserved */
#define GAP 1024ULL

static bool s_init = false;
static unsigned long long s_next;       /* next value to hand out */
static unsigned long long s_block_end;  /* persisted floor; reserve more when reached */

static void persist(unsigned long long floor) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u64(h, KEY, floor);
        nvs_commit(h);
        nvs_close(h);
    }
}

unsigned long long auth_counter_next(void) {
    if (!s_init) {
        unsigned long long stored = 0;
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u64(h, KEY, &stored);   /* leaves 0 if absent */
            nvs_close(h);
        }
        s_next = nem_auth_reserve(stored, GAP, &s_block_end);
        persist(s_block_end);
        s_init = true;
    }
    if (s_next >= s_block_end) {             /* exhausted the reserved block */
        (void)nem_auth_reserve(s_block_end, GAP, &s_block_end);
        persist(s_block_end);
    }
    return s_next++;
}
```

- [ ] **Step 3: Register in CMake**

In `firmware/main/CMakeLists.txt`, add `"auth_counter.c"` to the `SRCS` list, and add `mbedtls` to `REQUIRES` (used by Task 6; harmless here). The `REQUIRES` line becomes:

```cmake
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server mbedtls)
```

- [ ] **Step 4: Build to verify it compiles**

Run:
```bash
source ~/esp/idf-env.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -iE "error|Built target __idf_main" | grep -v kconfig
```
Expected: `Built target __idf_main`, no `error`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/auth_counter.h firmware/main/auth_counter.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): persistent monotonic request counter (NVS reservation)"
```

---

## Task 6: Firmware — request MAC + response verify in `net_fetch`

Replace the static Bearer token with the HMAC scheme, using mbedTLS. Add an on-boot known-answer self-test.

**Files:**
- Modify: `firmware/main/net_fetch.h`
- Modify: `firmware/main/net_fetch.c` (rewrite)
- Modify: `firmware/main/data_task.c` (derive key, use counter, run self-test)

**Interfaces:**
- Consumes: `nem_auth_req_message` (Task 1), `auth_counter_next` (Task 5).
- Produces:
  - `typedef struct { const uint8_t *key; unsigned long long counter; } nem_auth_t;` (key is 32 bytes or NULL for LAN mode).
  - `esp_err_t nem_http_get(const char *url, const nem_auth_t *auth, char *buf, size_t buf_sz, int *out_len);`
  - `bool nem_http_auth_selftest(void);`

- [ ] **Step 1: Rewrite the header**

Replace `firmware/main/net_fetch.h` with:

```c
#ifndef NET_FETCH_H
#define NET_FETCH_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* key = 32-byte SHA256(token), or key=NULL for LAN mode (no auth, no verify). */
typedef struct {
    const uint8_t     *key;
    unsigned long long counter;
} nem_auth_t;

/* HTTP GET into caller buffer (NUL-terminated). When auth->key is set: signs the
 * request (X-NEM-Ctr/X-NEM-Auth) and verifies the response signature (X-NEM-Sig),
 * failing closed. Returns ESP_OK only on HTTP 200 AND (LAN mode OR valid sig). */
esp_err_t nem_http_get(const char *url, const nem_auth_t *auth, char *buf, size_t buf_sz, int *out_len);

/* One-shot known-answer self-test of the on-device HMAC/base64 path. Logs + returns result. */
bool nem_http_auth_selftest(void);
#endif
```

- [ ] **Step 2: Rewrite `net_fetch.c`**

Replace `firmware/main/net_fetch.c` with:

```c
#include "net_fetch.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "nem/proxy_auth.h"

static const char *TAG = "fetch";

/* HMAC-SHA256(key[32], msg) -> base64 (44 chars + NUL) in out (>=45 bytes). */
static void hmac_b64(const uint8_t key[32], const uint8_t *msg, size_t len, char out[45]) {
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, msg, len, mac);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, 45, &olen, mac, 32);
    out[olen] = 0;
}

/* constant-time equality of two NUL-terminated strings of equal expected length */
static bool ct_eq(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na != nb) return false;
    int d = 0;
    for (size_t i = 0; i < na; i++) d |= (a[i] ^ b[i]);
    return d == 0;
}

esp_err_t nem_http_get(const char *url, const nem_auth_t *auth, char *buf, size_t buf_sz, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "nem-buddy/0.1",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

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

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return err; }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    int total = 0, r;
    while ((r = esp_http_client_read(c, buf + total, (int)buf_sz - 1 - total)) > 0) {
        total += r;
        if (total >= (int)buf_sz - 1) break;
    }
    buf[total < (int)buf_sz ? total : (int)buf_sz - 1] = 0;
    *out_len = total;

    esp_err_t result = (status == 200) ? ESP_OK : ESP_FAIL;
    if (result == ESP_OK && secured) {
        char *got = NULL;
        esp_http_client_get_header(c, "X-NEM-Sig", &got);
        char want[45];
        hmac_b64(auth->key, (const uint8_t *)buf, (size_t)total, want);
        if (!got || !ct_eq(got, want)) {
            ESP_LOGW(TAG, "response signature INVALID — rejecting");
            result = ESP_FAIL;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "GET %s -> %d, %d bytes%s", url, status, total, secured ? " (auth)" : "");
    return result;
}

bool nem_http_auth_selftest(void)
{
    uint8_t key[32];
    mbedtls_sha256((const unsigned char *)"testsecret", 10, key, 0);
    char msg[40], mac[45], sig[45];
    int mlen = nem_auth_req_message(msg, sizeof msg, 42ULL);
    hmac_b64(key, (const uint8_t *)msg, (size_t)mlen, mac);
    const char *body = "{\"t\":\"2026-07-14T00:00:00\",\"regions\":[]}";
    hmac_b64(key, (const uint8_t *)body, strlen(body), sig);
    bool ok = ct_eq(mac, "G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4=")
           && ct_eq(sig, "iYW1w5bFInoap2OfScY0Xt082rEedKXacCGM4EZu11g=");
    ESP_LOGI(TAG, "auth self-test: %s", ok ? "PASS" : "FAIL");
    return ok;
}
```

- [ ] **Step 3: Wire it into `data_task.c`**

In `firmware/main/data_task.c`: add includes near the top (after the existing includes):

```c
#include "auth_counter.h"
#include "mbedtls/sha256.h"
```

Replace the line:

```c
    const char *bearer = creds.proxy_token[0] ? creds.proxy_token : NULL;
```

with (derive the key once from the token):

```c
    uint8_t auth_key[32];
    bool secured = creds.proxy_token[0] != '\0';
    if (secured)
        mbedtls_sha256((const unsigned char *)creds.proxy_token, strlen(creds.proxy_token), auth_key, 0);
    nem_http_auth_selftest();
```

Add `#include <string.h>` if not already present (it is via other headers; add explicitly to be safe). Then replace the fetch call:

```c
        if (nem_http_get(creds.proxy_url, bearer, buf, PROXY_BUF_SZ, &len) == ESP_OK) {
```

with:

```c
        nem_auth_t auth = { .key = secured ? auth_key : NULL, .counter = auth_counter_next() };
        if (nem_http_get(creds.proxy_url, &auth, buf, PROXY_BUF_SZ, &len) == ESP_OK) {
```

- [ ] **Step 4: Build**

Run:
```bash
source ~/esp/idf-env.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -iE "error|Built target __idf_main" | grep -v kconfig
```
Expected: `Built target __idf_main`, no `error`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/net_fetch.h firmware/main/net_fetch.c firmware/main/data_task.c
git commit -m "feat(firmware): HMAC request auth + response verify in net_fetch (mbedTLS)"
```

---

## Task 7: Firmware — response freshness gate

Reject any parsed payload whose settlement epoch is strictly older than the last accepted one (response-replay defence).

**Files:**
- Modify: `firmware/main/data_task.c`

**Interfaces:**
- Consumes: `nem_auth_accept_fresh` (Task 1); `snap.regions[*].settlement_epoch` (already parsed).

- [ ] **Step 1: Add the freshness gate**

In `firmware/main/data_task.c`: add `#include "nem/proxy_auth.h"` near the includes. Add a static above `data_task`:

```c
static long long s_last_epoch = 0;
```

Inside the `for (;;)` loop, after `nem_proxy_parse(buf, &snap, &mix)` succeeds and **before** `bsp_display_lock(-1)`, insert:

```c
                long long ep = snap.regions[cfg.home_region].settlement_epoch;
                if (!nem_auth_accept_fresh(ep, s_last_epoch)) {
                    ESP_LOGW(TAG, "stale/replayed payload (epoch %lld <= %lld) — dropped", ep, s_last_epoch);
                    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
                    continue;
                }
                s_last_epoch = ep;
```

(The `continue` skips the UI update but keeps the poll loop running; it is not treated as a fetch error.)

- [ ] **Step 2: Build**

Run:
```bash
source ~/esp/idf-env.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -iE "error|Built target __idf_main" | grep -v kconfig
```
Expected: `Built target __idf_main`, no `error`.

- [ ] **Step 3: Commit**

```bash
git add firmware/main/data_task.c
git commit -m "feat(firmware): drop stale/replayed payloads via settlement-epoch freshness"
```

---

## Task 8: End-to-end verification (human-driven UAT)

Hardware + cluster + Cloudflare in the loop. The agent produces the commands; **the user flashes the board, applies the deploy, and sets the Cloudflare toggles.**

**Files:** none (verification only).

- [ ] **Step 1: Deploy the authenticated proxy**

Follow `deploy/RUNBOOK.md` §1–§5: create the secret (with a real OE key + a chosen `proxy-secret`), push the image, `kubectl apply`, add the cloudflared ingress, and set the Cloudflare toggles.

- [ ] **Step 2: Verify the public endpoint through Cloudflare**

Run (replace `<domain>` and `<secret>`):
```bash
echo "--- must be 401, and must NOT 301 to https ---"
curl -s -o /dev/null -w "%{http_code}\n" http://nembuddy.<domain>/nem
echo "--- valid request -> 200 + X-NEM-Sig ---"
CTR=999999
AUTH=$(python3 - "$CTR" <<'PY'
import hashlib,hmac,base64,sys
k=hashlib.sha256(b"<secret>").digest()
print(base64.b64encode(hmac.new(k,("GET /nem\n"+sys.argv[1]).encode(),hashlib.sha256).digest()).decode())
PY
)
curl -s -D - -o /dev/null "http://nembuddy.<domain>/nem" -H "X-NEM-Ctr: $CTR" -H "X-NEM-Auth: $AUTH" \
  | grep -iE "HTTP/|X-NEM-Sig|location"
```
Expected: `401` first (no `Location:`/no https redirect); then `200` with an `X-NEM-Sig` header and no `location:`. (Pick a `CTR` above what the device has likely used, or the proxy may 401 as a replay — restart the pod to reset if needed.)

- [ ] **Step 3: Provision the device and confirm on-device**

Re-provision via the captive portal: URL `http://nembuddy.<domain>/nem`, token = `<secret>`. Then capture serial (from `~/esp` env):
```bash
source ~/esp/idf-env.sh >/dev/null 2>&1 && python3 - <<'PY'
import serial,time
s=serial.Serial("/dev/cu.usbmodem21101",115200,timeout=0.2)
s.setDTR(False); s.setRTS(True); time.sleep(0.12); s.setRTS(False)
buf=b""; t0=time.time()
while time.time()-t0<20: buf+=s.read(4096)
s.close()
for l in buf.decode("utf-8","replace").splitlines():
    if any(k in l for k in ("auth self-test","fetch:","data: ok","signature","stale")): print(l[:160])
PY
```
Expected: `auth self-test: PASS`; `fetch: GET http://nembuddy... -> 200 ... (auth)`; `data: ok: ...`. No `signature INVALID`.

- [ ] **Step 4: Tamper test (negative)**

Temporarily break the proxy signature to prove the device fails closed: in the running pod, set the response signer to emit a wrong value (e.g. `kubectl set env deploy/nem-proxy NEM_PROXY_SECRET=wrongsecret` — now the proxy signs with a different key than the device). Re-capture serial: expect `response signature INVALID — rejecting` and the dashboard stops updating. Then restore the correct secret (`kubectl set env deploy/nem-proxy NEM_PROXY_SECRET=<secret>`).

- [ ] **Step 5: LAN regression (negative)**

Confirm nothing broke locally: run the proxy with **no** `NEM_PROXY_SECRET`, provision the device with an **empty** token + the LAN URL, and confirm `fetch: ... -> 200` (no `(auth)` suffix) and `data: ok`.

- [ ] **Step 6: Update project memory**

Note in memory that Plan 5 (authenticated internet proxy) is implemented + UAT'd on `feat/proxy-auth`, and that the proxy now requires `NEM_PROXY_SECRET` + the device token to match. Merge `feat/proxy-auth` → main per the finishing-a-development-branch flow.

---

## Self-Review

**Spec coverage:** §4 topology → Tasks 3–4; §5 wire protocol → Tasks 1 (message/fresh), 2 (proxy MAC/sign), 6 (device MAC/verify); §5.4 compression → Task 6 (`Accept-Encoding: identity`) + Task 2 (`no-transform`); §6 counter → Tasks 1 (reserve) + 5 (NVS); §7.1 proxy → Task 2; §7.2 container/k8s/CF → Tasks 3–4; §7.3 firmware → Tasks 5–7; §7.4 provisioning (reuse token) → Task 6 (`creds.proxy_token`) + RUNBOOK; §8 LAN mode → Tasks 2 + 6; §10 verification → Tasks 2/3 (local), 8 (E2E). All covered.

**Deviations from spec (intentional, better):** freshness uses the already-parsed `settlement_epoch` (integer compare) instead of the ISO string `t` — simpler and exact. The request-MAC path is the literal `/nem` on both ends (not derived from the URL) so URL-path config can't desync it.

**Placeholder scan:** none — all code is complete; `<domain>`/`<secret>`/`<registry>` in the RUNBOOK and Task 8 are genuine user-supplied values, not code placeholders.

**Type consistency:** `nem_auth_req_message`, `nem_auth_accept_fresh`, `nem_auth_reserve` signatures match between Task 1 (definition) and Tasks 5/6/7 (use). `nem_auth_t{key,counter}` and `nem_http_get(url, auth, buf, sz, len)` consistent between Task 6 header and `data_task` use. `auth_counter_next()` consistent Tasks 5↔6. Proxy `derive_key`/`sign_body`/`verify_request` consistent Tasks 2↔test.
