# Multi-Client Derived-Key Auth — Design

**Status:** Approved (brainstorming) — 2026-07-14
**Supersedes:** the single-device auth layer in
`docs/superpowers/specs/2026-07-14-nem-buddy-authenticated-internet-proxy-design.md`
(Plan 5), on the same branch `feat/proxy-auth` (not yet merged).

## Goal

Let **many** NEM Buddy devices (target scale: dozens to low hundreds, provisioned by
the operator — no public signup) read the shared plain-HTTP proxy over the public
internet, each with its **own** credential, without the single-device limitations of
Plan 5 (one shared secret, one in-memory replay counter, `replicas: 1`, and a
reflash-desync footgun).

## Key decisions (from brainstorming)

1. **Scale:** dozens–low hundreds, operator-provisioned. Not a load problem — one
   proxy replica polling upstream on a timer serves this trivially (a few req/s).
2. **Credential model:** per-device secret + explicit device id. Individual
   revocation; one leaked device does not compromise others.
3. **Replay/state:** **stateless auth.** Drop the strict request counter. Data
   integrity is already guaranteed by the signed response + the on-device
   `settlement_epoch` freshness gate. The proxy keeps **no** per-device state.
4. **Key storage:** **derive** each device key from one master secret. The proxy
   stores only the master secret; it never holds a device list.

## Architecture

The proxy remains a single **fan-out cache**: one OpenElectricity key, a background
thread refreshing AEMO (~60 s) + OE (~5 min) into one in-memory payload, served to all
devices. Multi-client is a change to the **auth layer only** — no change to how data is
fetched, merged, or cached.

### Key hierarchy

```
master_key  = SHA256(master_secret)                  # 32 bytes, proxy-only (one env var, NEM_PROXY_SECRET)
device_key  = HMAC-SHA256(master_key, device_id)     # 32 bytes, per device; computed offline, stored on the device
```

- The proxy stores **only** `master_secret`. Given any request's `device_id`, it
  re-derives that `device_key` on the fly — no stored device map, no datastore.
- The device is provisioned with its own `device_id` + `device_key`. It never sees
  `master_secret`.
- `device_id` is a human-friendly label (e.g. `rjk-kitchen`). Its secrecy is not
  relied upon; security rests entirely on `device_key` (which requires `master_secret`
  to derive).

### Wire protocol (v2)

**Request (device → proxy):**

| Header | Value |
|---|---|
| `X-NEM-Id` | `device_id` (UTF-8, in clear) |
| `X-NEM-Auth` | `base64(HMAC-SHA256(device_key, "GET /nem"))` |
| `Accept-Encoding` | `identity` |

The MAC is over the **constant** message `"GET /nem"` (no counter, no clock). It is a
static per-device authenticator that proves possession of `device_key` **without ever
transmitting it**.

**Response (proxy → device):**

| Header | Value |
|---|---|
| `X-NEM-Sig` | `base64(HMAC-SHA256(device_key, body))` — signed with the requesting device's key |
| `Cache-Control` | `no-store, no-transform` |

Base64 is standard (with `=` padding). The response body is byte-identical across
devices (same public NEM data); only the per-device signature differs, so responses
must **not** be shared between devices — `no-store` enforces this at any intermediary
cache (Cloudflare).

### Request handling (proxy `do_GET`)

1. If `master_secret` is unset → **LAN mode**: serve without any auth check or
   `X-NEM-Sig` (unchanged legacy behaviour).
2. Read `X-NEM-Id`. If absent → 401.
3. If `device_id` is in the denylist → 401.
4. `device_key = HMAC-SHA256(master_key, device_id)`.
5. Verify `X-NEM-Auth == base64(HMAC-SHA256(device_key, "GET /nem"))`, constant-time.
   Mismatch → 401.
6. Serve the cached payload; add `X-NEM-Sig = base64(HMAC-SHA256(device_key, body))`
   and the cache headers. (503 with no signature while warming up, as today.)

No `_last_ctr`, no per-device state, no lock needed for auth.

## Threat model (plain-HTTP, MITM-capable)

- **Forged/altered data:** blocked. A MITM cannot compute a valid `X-NEM-Sig` for a
  modified body without `device_key`; the device fails closed on a bad/absent sig.
- **Stale replayed response:** blocked by the on-device `settlement_epoch` freshness
  gate (reject any payload strictly older than the last accepted). Residual:
  `s_last_epoch` is RAM-only and resets to 0 on reboot, so immediately after a device
  reboot one previously-captured, validly-signed-for-that-device stale response could
  be accepted once before the next live poll (newer epoch) supersedes it; impact is
  bounded (briefly-stale public data, no side effects), an accepted residual.
- **Replayed request:** possible but harmless — returns the same public data; no side
  effects, no OE-quota cost (quota is time-based).
- **Unprovisioned access / scraping:** blocked. A party without a valid `device_key`
  cannot produce a valid `X-NEM-Auth` for any `device_id`.
- **Cross-device compromise:** contained. Leaking one `device_key` does not reveal
  `master_secret` or any other `device_key`. Revoke via denylist + re-provision under a
  new id.
- **Residual (accepted):** a captured request authenticator is a static per-device
  bearer replayable over plain HTTP. Accepted because replay yields only public data.
  A future coarse time-window binding (still stateless) can limit this if desired;
  out of scope here.

## Provisioning

**Helper (new, `proxy/provision_device.py`):**

```
$ NEM_PROXY_SECRET=<master> python3 proxy/provision_device.py rjk-kitchen
device_id:  rjk-kitchen
device_key: j4etZdu/JxavgbiTMu+9AJ1npkpySZzlsgTfeHxBSEw=   (base64, 32 bytes)
```

Derives `device_key = HMAC(SHA256(master), device_id)` and prints the two values to
enter into the device captive portal. Adding a device is zero-touch on the proxy — no
redeploy, no secret edit.

**Captive portal + creds (firmware):** the single "Proxy token" field becomes two —
**Device ID** and **Device key** (base64):

- `device_id` → new NVS string field in `net_creds`.
- `device_key` → the base64 value is stored in the existing `proxy_token` NVS slot
  (reused to avoid NVS schema churn; renamed in-struct for clarity). Firmware
  base64-decodes it to 32 raw bytes at load.

## Firmware auth path

- Load `device_id` (string) and `device_key` (32 raw bytes, base64-decoded from creds).
  No on-device key derivation — the key arrives ready to use (simpler than Plan 5,
  which SHA256'd a token on-device).
- Send `X-NEM-Id` and `X-NEM-Auth = HMAC(device_key, "GET /nem")`, plus
  `Accept-Encoding: identity`.
- Verify `X-NEM-Sig` with `device_key`, **fail closed** on absent/mismatch.
- Keep the `settlement_epoch` freshness gate unchanged.
- **Remove** `auth_counter.c/.h` and its NVS counter entirely.

**LAN mode:** empty `device_id`/`device_key` on the device + no master on the proxy →
no auth headers, no verify. A device with a key set but a missing/bad `X-NEM-Sig` still
fails closed.

## Revocation

Env `NEM_PROXY_DENY="alice-01,bob-03"` (comma-separated `device_id`s). `do_GET` rejects
a listed id with 401 before deriving/verifying. To fully rotate a compromised device,
add its id to the denylist and re-provision it under a new id.

## Deployment

Auth now holds **zero** per-device state, so nothing in the auth layer forces
`replicas: 1` / `strategy: Recreate`. The payload cache and OE polling are still
per-pod, so N replicas = N× OE cadence; since one replica serves low-hundreds of
devices, **keep `replicas: 1`** — but as an *OE-quota* choice, not an auth constraint.
The manifest comment is updated to say so. RUNBOOK gains the `provision_device.py` step
and the `NEM_PROXY_DENY` env.

## Delta from the `feat/proxy-auth` branch (Plan 5)

- **Remove:** `X-NEM-Ctr`; `firmware/main/auth_counter.{c,h}`; the proxy `_last_ctr`
  state; `nem_auth_reserve` (core) and its test; the counter-reservation NVS logic.
- **Change:** core message helper → the constant `"GET /nem"` (no counter arg); proxy
  `derive_key`/`sign_body`/`verify_request` → per-device derived keys keyed on
  `X-NEM-Id`; firmware `net_fetch`/`data_task`/portal/`net_creds` → send `X-NEM-Id`,
  load a ready-made `device_key`, drop the counter.
- **Keep:** response signing, the `settlement_epoch` freshness gate, LAN mode, the
  Dockerfile, and the k8s manifest (comment tweak only).
- Net result: **less** code than the current branch.

## Testing

**Known-answer vector (cross-language — copy exactly):**

- `master_secret = "testmaster"`
- `master_key` (hex) = `d9095f2c47b0a651748521077659d03539f4376f17ed79ef070c32d2f6198fb1`
- `device_id = "dev01"`
- `device_key` (hex) = `8f87ad65dbbf2716af81b89332efbd009d67a64a72499ce5b204df787c41484c`
- `device_key` (base64) = `j4etZdu/JxavgbiTMu+9AJ1npkpySZzlsgTfeHxBSEw=`
- message `"GET /nem"` → `X-NEM-Auth` = `AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=`
- body `{"t":"2026-07-14T00:00:00","regions":[]}` → `X-NEM-Sig` =
  `uPGokXyiUFAQFM1FjIK8dW3EGa3Rgm23GDpxFIc+VGk=`
- second device `device_id = "dev02"` → `device_key` (hex) =
  `90eafea9d5492e7ef1d3201c6453c612a70f33d98786bb029f642dc1f6e712a3` (for
  wrong-device / denylist tests)
- RFC 4231 case 2 (mbedTLS sanity): HMAC-SHA256(key=`Jefe`,
  `what do ya want for nothing?`) =
  `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`

**Core:** message-constant test; keep the `nem_auth_accept_fresh` freshness test; drop
the reserve test.

**Proxy (`test_auth.py`):** derive `device_key` from the test master + id; valid
`X-NEM-Auth` → verifies true; `dev02`'s key against `dev01`'s expected → false;
denylisted id → rejected by the handler path.

**Firmware:** on-boot known-answer self-test updated to this vector (Python `hmac` vs
mbedTLS agreement).

**Integration + E2E (human UAT):** curl with `X-NEM-Id` + computed auth → 200 +
`X-NEM-Sig`; bad/absent auth → 401; denylisted id → 401. On hardware: two devices with
distinct ids both updating; revoke one via `NEM_PROXY_DENY`; confirm it 401s while the
other keeps updating; LAN-mode regression.

## Out of scope

Public self-serve signup, user accounts/billing, per-device OE keys, durable per-device
state / Redis, multi-replica horizontal scaling, and coarse time-window request binding.
Each can layer on later without reworking this design.
