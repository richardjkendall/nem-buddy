# NEM Buddy — Authenticated Internet-Reachable Proxy

**Date:** 2026-07-14
**Status:** Approved design, pending implementation plan
**Related:** `2026-07-10-nem-buddy-realtime-monitor-design.md`, memory `nem-data-architecture`

## 1. Motivation

Today the device reads its data from a plain-HTTP proxy on the **local LAN**
(`proxy/nem_proxy.py`). The device cannot speak TLS — WiFi + the AMOLED flush DMA +
an mbedTLS session together exceed the board's ~53 KB of internal DMA SRAM (see
`nem-data-architecture`). Keeping the proxy LAN-only avoids exposing plain HTTP to
the internet, but ties the device to a machine on the same network.

We want the proxy **reachable over the internet** (hosted on the user's existing
Kubernetes cluster, exposed via a Cloudflare Argo tunnel) so the device works on any
network, without a local always-on host. Exposing plain HTTP publicly requires an
**application-layer authentication scheme** to replace the transport security the
device can't do.

## 2. Goals / Non-goals

**Goals**
- Proxy reachable over the public internet as **plain HTTP** (device is HTTP-only).
- **Response integrity/authenticity:** the device rejects any tampered or spoofed
  payload — a man-in-the-middle cannot feed it fake prices or trigger false alerts.
- **Request authenticity + replay resistance:** only holders of the shared secret can
  make the proxy do work (protects the OpenElectricity API key/quota); captured
  requests cannot be replayed.
- **Backward compatible:** one firmware build and one proxy work both LAN (no secret →
  no auth) and internet (secret set → full auth).

**Non-goals**
- **Confidentiality.** NEM data is public; we do not encrypt the payload. The scheme is
  authentication + integrity only (a MAC, not encryption).
- **Multi-device.** The counter scheme assumes **one device per shared secret** (see
  §8). Provisioning many devices with distinct secrets is out of scope.
- **Asymmetric keys.** We start with a symmetric pre-shared key (device holds a key that
  could forge). Ed25519 (device holds only a public key) is noted as future hardening
  (§9), not built now.

## 3. Threat model

The only public-internet plaintext hop is **device ↔ Cloudflare edge**. Cloudflare ↔
origin is carried by the encrypted cloudflared tunnel.

| Threat | Mitigation |
|---|---|
| MITM tampers the response (fake prices → false alerts) | Origin signs the body with `X-NEM-Sig`; Cloudflare does not hold the key, so integrity is end-to-end origin→device. |
| MITM replays an older valid response | Device rejects payloads whose settlement `t` is **older** than the last accepted one. |
| Unauthorised use of the proxy (burn OE quota) | Request must carry a valid `X-NEM-Auth` MAC over a monotonic counter; Cloudflare edge rate-limiting as defence in depth. |
| Replay of a captured request | Proxy rejects a counter ≤ the last it accepted. |
| Eavesdropping the payload | Out of scope — data is public. |
| Shared-secret extraction from the device | Accepted risk for symmetric start; Ed25519 removes it later (§9). |

## 4. Architecture

```
device (HTTP only)  ──http──▶  Cloudflare edge  ──cloudflared tunnel (encrypted)──▶  k8s Service :8080 ──▶ proxy pod
        ▲  X-NEM-Ctr / X-NEM-Auth  (request MAC + counter)
        └────────────────  X-NEM-Sig  (response MAC over body)  ◀────────────────────────────────────────
```

- **Proxy pod:** the existing `proxy/nem_proxy.py`, containerised. Keeps its in-memory
  intraday history + 60 s poll loop, so **`replicas: 1`** (multiple replicas would hold
  divergent history and divergent counter state). Backfill from OE self-heals history on
  restart.
- **k8s:** Deployment (1 replica) + ClusterIP Service on 8080. Secrets `NEM_OE_API_KEY`
  (existing) and `NEM_PROXY_SECRET` (new).
- **cloudflared:** ingress rule `nembuddy.<domain>` → `http://<proxy-svc>:8080`.
- **Cloudflare zone config** for the hostname:
  - **"Always Use HTTPS" = off**, no HSTS, no Page/Configuration Rule forcing HTTPS —
    so `http://nembuddy.<domain>/nem` is served without a 301 the device can't follow.
  - Cache **bypass** (respond `Cache-Control: no-store`), no content optimisation
    (minify/Rocket Loader off) so the JSON body is byte-identical for the MAC.
  - Optional rate-limit rule on the hostname.

## 5. Wire protocol

Shared secret is the provisioning **token** (reused as a passphrase). Both sides derive:

```
key = SHA256( token_utf8 )          # 32 bytes
```

### 5.1 Request (device → proxy)
- `X-NEM-Ctr: <n>` — decimal, a monotonic counter that never repeats or decreases
  across reboots (see §6).
- `X-NEM-Auth: base64( HMAC-SHA256(key, "GET " + path + "\n" + <n>) )`
  e.g. message `"GET /nem\n42"`.

Proxy: recompute the MAC from `method`, request `path`, and `X-NEM-Ctr`; constant-time
compare. Reject (HTTP 401) if the MAC is wrong **or** `n ≤ last_seen`. On success set
`last_seen = n`. `last_seen` is per-secret, in memory.

### 5.2 Response (proxy → device)
- Body: the existing compact JSON, **unchanged**.
- `X-NEM-Sig: base64( HMAC-SHA256(key, body_bytes) )`
- `Cache-Control: no-store, no-transform`

Device: after reading the body bytes, recompute the MAC and constant-time compare to
`X-NEM-Sig`. Reject the response on mismatch **or** (fail-closed) if a token is set but
`X-NEM-Sig` is absent.

### 5.3 Freshness (response replay)
The payload already carries `"t"` (AEMO settlement time, ISO-8601). Device keeps
`last_accepted_t`. Rule: **reject if `t < last_accepted_t`** (strictly older). `t ==
last` is allowed (a normal no-change poll — AEMO updates every 5 min, device polls every
60 s); it is accepted as a no-op and does not error. ISO-8601 lexical order equals
chronological order, so a string compare suffices.

### 5.4 Compression
Device sends `Accept-Encoding: identity` so Cloudflare returns the body uncompressed and
the bytes the device MACs match the bytes the proxy signed.

## 6. Request counter (clock-free, low NVS wear)

The device has no reliable wall clock at request time, so replay resistance uses a
persistent monotonic counter instead of a timestamp.

- Stored in NVS as a 64-bit value `auth_ctr`.
- **Reservation to bound NVS writes:** on boot, read stored floor `F`, set in-RAM
  counter to `F`, persist `F + GAP` (GAP = 1024) as the new floor. Use `++counter`
  per request from RAM (no per-request NVS write). When the RAM counter reaches the
  persisted floor, persist the next `+GAP` block. Result: ~1 NVS write per 1024
  requests (≈ once/day at a 60 s poll), and the counter is strictly increasing across
  reboots (reboots skip forward, never back).
- 64-bit space is effectively inexhaustible at this rate.

Proxy `last_seen` resets on pod restart. Consequence: immediately after a proxy restart,
one previously-captured request could be replayed once until the device's counter
advances past it. This is low-impact (a replayed request only returns public data) and
is accepted; optionally the proxy may persist `last_seen` to survive restarts (noted,
not required).

## 7. Component changes

### 7.1 Proxy (`proxy/nem_proxy.py`)
- Read `NEM_PROXY_SECRET` from env; `key = sha256(secret)`. If unset → **LAN mode**:
  do not require `X-NEM-Auth`, do not add `X-NEM-Sig` (current behaviour unchanged).
- If set: verify request MAC + counter (§5.1) before serving; on failure return 401 and
  do no upstream work. Add `X-NEM-Sig` (§5.2) and `Cache-Control: no-store, no-transform`
  to responses. Track `last_seen` in memory.
- Constant-time compare (`hmac.compare_digest`).

### 7.2 Container + k8s + Cloudflare
- `Dockerfile` (`python:3-slim`, copy `proxy/`, run `nem_proxy.py --port 8080`).
- k8s manifests: Deployment (`replicas: 1`), Service (ClusterIP :8080), Secret refs
  (`NEM_OE_API_KEY`, `NEM_PROXY_SECRET`).
- cloudflared ingress snippet for `nembuddy.<domain>`.
- A short runbook: Cloudflare zone toggles (§4), secret creation, deploy, verify.

### 7.3 Firmware
- `net_fetch.c`: replace the static `Authorization` header. Given the token:
  derive `key = SHA256(token)`; send `X-NEM-Ctr` + `X-NEM-Auth` and
  `Accept-Encoding: identity`; after `esp_http_client_fetch_headers`, read `X-NEM-Sig`
  via `esp_http_client_get_header`; recompute `HMAC-SHA256(key, body)` over the received
  bytes (mbedTLS `mbedtls_md_hmac`, SHA-256) and constant-time compare. Fail closed when
  a token is set but the signature is missing/wrong. Empty token → LAN mode (no headers,
  no verification).
- Counter store: NVS `auth_ctr` with the reservation scheme (§6).
- Freshness: in `data_task`, after a successful parse, compare payload `t` to the stored
  `last_accepted_t`; drop strictly-older payloads without treating them as a fetch error.
- HMAC/SHA live in mbedTLS (already in the build); buffers are a few dozen bytes — far
  under the internal-RAM ceiling that blocks TLS.

### 7.4 Provisioning
No new field. The existing **"Proxy token (optional)"** portal field / NVS `tok` is the
shared secret. The proxy URL field takes the Cloudflare hostname
(`http://nembuddy.<domain>/nem`). UI copy may be relabelled to reflect that the token is
now a shared secret when using a public proxy.

## 8. Backward compatibility & LAN mode

- **Proxy:** `NEM_PROXY_SECRET` unset → behaves exactly as today (no request auth, no
  signature). Set → enforces auth.
- **Device:** token empty → sends no auth headers and does not require a signature (LAN).
  Token set → full auth, fail-closed.
- The two combine so the same artefacts serve LAN dev and the public deployment; the only
  switch is whether a secret/token is configured on both ends.
- **One device per secret** (the proxy tracks a single `last_seen`). Multiple devices
  sharing a secret would have interleaving counters and reject each other.

## 9. Security notes & future hardening

- Symmetric PSK: device compromise leaks a key that can forge responses and mint
  requests. Acceptable for a single hobby device. **Future:** proxy holds an Ed25519
  private key and signs responses; device holds only the public key and verifies —
  device compromise can no longer forge responses. Request auth would then need a
  per-device key.
- Cloudflare is a trusted intermediary by design but cannot forge (no key). Ensure no CF
  feature rewrites the body (minify/optimisation off, `no-transform`).
- Rate-limit at the Cloudflare edge as defence in depth for quota abuse.

## 10. Verification

1. **Known-answer test** (host): a fixed `(token, counter, path, body)` vector; assert
   the Python proxy's HMACs and an independent reference agree, so the C firmware
   computation can be checked against the same vector.
2. **Proxy local:** run with a secret; `curl` with correct vs wrong/absent `X-NEM-Auth`
   → 200 vs 401; assert `X-NEM-Sig` present and correct; replay a counter → 401.
3. **Device:** provision internet URL + token = secret; confirm fetch + verify via serial
   log. Tamper test: force a wrong `X-NEM-Sig` on the proxy → device rejects (logged).
4. **Through Cloudflare:** confirm `http://` serves without redirect, the body is
   byte-identical (identity encoding), and `X-NEM-Sig` still verifies end-to-end.
5. **LAN regression:** no secret/token → device and proxy behave exactly as today.

## 11. Open questions

None outstanding. (Optional/deferred: persisting proxy `last_seen` across restarts;
Ed25519 hardening; multi-device secrets.)
