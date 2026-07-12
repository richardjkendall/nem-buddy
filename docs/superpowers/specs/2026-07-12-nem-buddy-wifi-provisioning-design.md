# NEM Buddy — WiFi Provisioning (Plan 3b) Design

**Date:** 2026-07-12
**Status:** Approved design (pre-implementation)
**Supersedes:** compile-time WiFi/proxy config in `firmware/main/secrets.h`
**Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16 (ESP-IDF v5.5 + LVGL v9)

## Problem

WiFi credentials and the proxy URL are currently hardcoded in the gitignored
`firmware/main/secrets.h` and compiled in. Changing networks means editing a file
and reflashing. Plan 3b replaces this with on-device provisioning: a SoftAP captive
portal that captures WiFi + proxy config, persists it to NVS, and connects.

## Decisions

1. **Trigger — self-healing fallback.** Enter the portal when NVS has no creds *or*
   when an STA connect attempt fails after **5** retries. A wrong password or a vanished
   network automatically falls back to the portal on the next boot. No manual factory
   reset required for the common re-provision case.
2. **True captive portal.** SoftAP + HTTP server + a minimal DNS server that resolves
   every A query to `192.168.4.1`, so phones auto-pop the "Sign in to network" sheet.
3. **Scan-and-pick SSID.** The device scans nearby networks and the form offers a
   dropdown, with a "join other / hidden" free-text fallback.
4. **Proxy URL + optional token.** The form captures the proxy URL (optional, prefilled
   with the last-saved value) and an optional proxy auth token, both persisted to NVS.
   The token is sent as `Authorization: Bearer <token>` (via the `bearer` arg
   `net_fetch.c` already supports). Public NEM prices are not confidential, so the
   token exists to protect a future internet-exposed proxy from **abuse** (burning the
   OpenElectricity API quota), not for payload confidentiality.
5. **`secrets.h` as optional first-boot seed.** NVS is the source of truth. If NVS is
   empty and `secrets.h` defines creds, seed NVS from them on first boot (zero-friction
   dev flow). Absent/empty macros → normal portal flow. `secrets.h.example` stays as
   documentation.
6. **Reboot-after-save transition.** On form submit: validate → write NVS → show
   "Saved — connecting…" → `esp_restart()`. The clean boot re-enters the state machine
   with creds present and connects STA. Avoids flaky in-place `APSTA→STA` mode
   transitions on this RAM-constrained board.
7. **WPA2 setup AP with a fixed password.** The portal AP (`NEM-Buddy-XXXX`) is WPA2-PSK
   with a fixed compile-time password (≥8 chars). Both the AP name and password are
   displayed on the on-screen setup card so the user can join. Keeps the setup network
   off open-network scanners.

## Out of scope (future plans)

- **Internet-reachable proxy deployment** (Pi/VPS), rate limiting, and token
  enforcement on the proxy side. Provisioning is made forward-compatible (URL scheme +
  token captured), so deployment will not require re-provisioning devices.
- **On-device TLS spike.** On-device HTTPS was ruled out because reading the ~11 KB TLS
  response *directly from AEMO/OE* did not fit alongside WiFi + panel DMA in ~53 KB of
  internal DMA RAM. TLS to our **own** proxy (which already trims to ~740 bytes), with
  `MBEDTLS_SSL_IN/OUT_CONTENT_LEN` reduced to ~2–4 KB and mbedtls buffers in PSRAM, is
  a different and plausible problem — worth a dedicated future spike, not part of 3b.
- On-screen settings (region/thresholds, manual re-provision gesture) remain Plan 4.

## Architecture

New `firmware/main/net/` glue modules plus small pure-C helpers in `core/` (host-tested,
matching the Plan-1 pattern).

| Unit | Type | Responsibility | Depends on |
|---|---|---|---|
| `nem_provision` (core) | pure C, host-tested | Parse URL-encoded form body → ssid/pass/proxy_url/token; validate/trim; build captive-DNS reply packet | stdlib |
| `net_creds` | ESP glue | Load/save creds struct to NVS namespace `nem`; seed from `secrets.h` if NVS empty | `nvs_flash` |
| `wifi_ctrl` | ESP glue | One-time wifi/netif/event init; `sta_connect(creds, retries)`; `ap_start(name)`/`ap_stop`; `scan()`→SSID list | `esp_wifi`, `esp_netif`, `esp_event` |
| `captive_dns` | ESP glue | UDP:53 task answering every A-query with 192.168.4.1 (uses core packet builder) | lwip sockets, `nem_provision` |
| `portal_http` | ESP glue | `esp_http_server`: serve setup form (scan list + prefilled proxy fields), handle POST submit, OS captive-detect endpoints | `esp_http_server`, `net_creds`, `wifi_ctrl` |
| `net_manager` | ESP glue | Orchestrator/state machine; entrypoint replacing `wifi_sta_connect()`'s role | all above |

`wifi_sta.c` is refactored: its init logic splits into `wifi_ctrl` (reused by STA and
AP); NVS init moves under `net_creds`/`net_manager`.

## Boot state machine

```
BOOT
 └─ net_creds_load()  (NVS; if empty, seed from secrets.h)
      ├─ creds present ──► STA_CONNECT (up to 5 retries)
      │                      ├─ success ──► CONNECTED ──► data_task fetch loop
      │                      └─ fail ─────► PORTAL
      └─ no creds ─────────────────────────► PORTAL

PORTAL
 ├─ wifi_ctrl.scan()  → cache nearby SSIDs
 ├─ wifi_ctrl.ap_start("NEM-Buddy-XXXX", WPA2 fixed pw)  (XXXX = last 2 MAC bytes)
 ├─ captive_dns start  (all A → 192.168.4.1)
 ├─ portal_http start  (form w/ scan list + prefilled proxy url/token)
 ├─ UI: setup card (AP name + password + "connect & open http://192.168.4.1")
 └─ on POST /save:
       validate → net_creds_save() → UI "Saved — connecting…" → esp_restart()
```

- `main.c` renders the dashboard shell, then calls `net_manager_start()`.
- `net_manager` invokes `data_task_start()` only after reaching CONNECTED — this
  replaces today's `700ms` race hack, since WiFi is now brought up deliberately.
- The fetch loop reads `proxy_url` + `proxy_token` from `net_creds` (token → `bearer`)
  instead of the `secrets.h` macros.
- AP is WPA2-PSK with a fixed compile-time password (shown on the setup card); the form
  POST over that link is the only write path and the payload is non-sensitive.

## Portal HTTP

**Endpoints (`esp_http_server`):**
- `GET /` → self-contained setup form (inline CSS, no external assets).
- OS captive-probe URLs → `/`: `/generate_204`, `/gen_204` (Android);
  `/hotspot-detect.html`, `/library/test/success.html` (iOS/macOS);
  `/connecttest.txt`, `/ncsi.txt` (Windows).
- `POST /save` → parse, validate, persist, respond "Saved — connecting…", then
  `esp_restart()` (~1 s later so the response flushes).

**Form fields:** SSID `<select>` from scan cache + "join other / hidden" free-text;
password (empty allowed); proxy URL (prefilled, optional); proxy token (prefilled,
optional).

**Validation (in host-tested `nem_provision`):** URL-decode
`application/x-www-form-urlencoded`; enforce caps (SSID ≤32, pass ≤64, url ≤128,
token ≤128 — sized to NVS / `wifi_config_t`). SSID required; rest optional. Reject
over-length/garbage → re-render form with inline error (stay in PORTAL).

## Error handling

- Empty scan → form renders with only the free-text SSID path.
- STA connect fail after provisioning → reboot lands back in PORTAL (self-heal).
- NVS write failure → surface error on form, do not reboot.
- Proxy URL blank when CONNECTED → dashboard shows a "no proxy configured" state; WiFi
  stays up.
- RAM: portal runs while the fetch loop / dashboard updates are idle (static setup
  card), so DNS+HTTP+AP coexist in the headroom the fetch loop normally uses.

## Testing

**Host-tested (`core/`, existing runner):**
- `nem_provision_parse_form()` — happy path, `+`/`%XX` decode, missing SSID,
  over-length fields, empty optionals, embedded `&`/`=`, truncation caps.
- `nem_provision_build_dns_reply()` — byte-level assertions on a well-formed A-record
  answer → 192.168.4.1 (header flags, answer count, RDATA).
- `net_creds` seed-from-`secrets.h` precedence logic reviewed (NVS round-trip itself is
  on-board, not host-tested).

**On-board UAT (manual, flash & observe):**
1. Fresh flash, NVS empty, no `secrets.h` creds → PORTAL; `NEM-Buddy-XXXX` visible;
   card shows the WPA2 password; joining with it auto-pops the captive sheet; scan list
   shows real networks.
2. Submit good creds → "Saved — connecting…" → reboot → CONNECTED → dashboard live.
3. Submit wrong password → reboot → STA fails → back to PORTAL automatically.
4. Power-cycle after success → straight to CONNECTED (NVS persisted).
5. `secrets.h` present, NVS empty → seeds and connects without portal (dev flow).
6. Panel renders correctly through AP-up and STA-up (DMA/WiFi coexistence).
