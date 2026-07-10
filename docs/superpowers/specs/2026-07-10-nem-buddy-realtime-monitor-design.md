# NEM Buddy — Real-time NEM Monitor Design

**Date:** 2026-07-10
**Status:** Approved design, ready for implementation planning
**Target hardware:** Waveshare ESP32-S3-Touch-AMOLED-2.16

## 1. What we're building

An always-on desk display that shows the live state of Australia's National Electricity
Market (NEM) and comes alive when something dramatic happens on the grid. It is two things
at once:

- **An ambient grid dashboard** — the current spot price, demand and generation mix for a
  home region (Victoria by default, configurable), with a glance strip for the other four
  regions.
- **A drama alerter** — when a notable event fires (price spike, negative price, extreme
  demand, big renewable moment) the screen takes over with a bold full-screen card and a
  chime, then returns to the dashboard.

It is *not* tied to a personal electricity bill — it uses public data only, so there is no
account login or wholesale-plan dependency.

## 2. Hardware baseline (confirmed)

| Component | Part | Notes |
|---|---|---|
| SoC | ESP32-S3R8 (dual LX7, 240 MHz) | 8 MB PSRAM, 16 MB flash |
| Display | 2.16" AMOLED, 480×480, CO5300 driver (QSPI) | True black, vivid accents |
| Touch | CST9220 capacitive | Tap + swipe |
| Audio out | ES8311 codec → mono speaker, PA enable on GPIO 9 | Chime playback |
| Other | 6-axis IMU, RTC | Not required by v1; RTC aids timekeeping |

PSRAM at 8 MB comfortably covers LVGL framebuffers plus per-region intraday history buffers.

## 3. Firmware stack

- **ESP-IDF** (native), not Arduino.
- **LVGL** via `esp_lvgl_port` for all UI (screens, charts, swipe gestures, alert cards).
- **`esp_wifi`** for STA + SoftAP; **`esp_http_server`** + a captive-portal DNS responder for
  WiFi setup.
- **NVS** for persisted config (WiFi credentials, home region, alert thresholds, brightness,
  quiet hours).
- **ES8311 over I2S** for the chime (thin direct driver or esp-adf), toggling the PA on GPIO 9
  around playback to avoid pops.
- Board bring-up starts from Waveshare's ESP-IDF board-support demos (CO5300 / CST9220 /
  ES8311 drivers).

## 4. Data source & architecture

**Direct-to-API from the device** — no backend server to run or maintain. The data layer is
written behind a single interface so a proxy can be introduced later without touching the UI.

Two public sources, both free and login-free:

- **AEMO `ELEC_NEM_SUMMARY`** (compact JSON) — live per-region spot price, demand and net
  interconnector flow. Small and device-friendly; this drives the headline numbers and the
  glance strip. Polled every **60 s**.
- **OpenNEM API** — intraday price/demand history and generation mix by fuel type per region.
  Larger payloads; used for the drill-in screens. Refreshed every **5 min** (NEM dispatch
  cadence).

Streaming JSON parsing keeps peak memory bounded; only the fields the UI needs are extracted
into normalized structs.

### Alert thresholds (defaults, all configurable on-device)

| Event | Default trigger |
|---|---|
| Price spike | region price > **$300/MWh** (extreme styling above $1,000) |
| Negative price | region price < **$0/MWh** |
| Extreme demand | region demand above its configured high-water mark |
| High renewable moment | region renewable share > **80%** |

Alerts fire for **any** region, not just home. Each event type is debounced (no re-alert while
the condition persists) and respects **quiet hours** (chime suppressed; visual still allowed
per config).

## 5. Components (isolated units)

Each unit has one purpose, a defined interface, and can be reasoned about on its own.

1. **`board` (platform init)** — brings up display (CO5300 QSPI), touch (CST9220), LVGL port,
   audio codec, WiFi radio, and time (SNTP → RTC). Exposes initialized handles; knows nothing
   about NEM data.
2. **`config`** — NVS-backed store for WiFi credentials, home region, thresholds, quiet hours,
   brightness. Load/save + change notifications. The single source of tunables.
3. **`provisioning`** — SoftAP + captive portal. When `config` has no WiFi credentials, starts
   AP `NEM-Buddy-XXXX`, serves a setup form (SSID/password, optionally home region + a couple
   of thresholds), writes to `config`, then hands back to STA connect. Also re-entrant from
   Settings ("Change WiFi").
4. **`nem_client` (data layer)** — the swappable interface. Fetches AEMO summary + OpenNEM,
   parses (streaming JSON), and returns normalized snapshots. Two concrete sources today; a
   proxy source can be dropped in later. No UI or alert logic here.
5. **`store` (state model)** — holds the current snapshot for all five regions plus per-region
   intraday ring buffers (price, demand, mix). Publishes updates to observers (UI + alert
   engine). Owns "stale" / "last updated" age.
6. **`alerts` (event engine)** — evaluates each new snapshot against thresholds, applies
   debounce + quiet hours, and emits events. Drives the takeover screen and requests a chime.
7. **`audio`** — plays named chimes through ES8311 (enable PA → play → disable). Distinct tones
   for spike vs negative vs demand. Honors quiet hours + a mute setting.
8. **`ui` (LVGL screens)** — all rendering & navigation (details in §6). Observes `store`,
   listens for `alerts` events, reads/writes `config` for the settings screen.

### Data flow

```
board init → config.load()
   ├─ no WiFi creds → provisioning (SoftAP + setup screen) → save creds → reconnect
   └─ have creds   → STA connect → SNTP time sync
                        → poll timers (60s live / 5min history)
                            → nem_client.fetch() → normalize → store.update()
                                 ├─ ui redraws (observer)
                                 └─ alerts.evaluate() → [event] → takeover screen + audio.chime()
                                                                     → auto-return after ~30s
```

## 6. Screens (UI)

Visual language: true-black background; **blue = normal, green = cheap/negative,
amber→red = high/spike**. Live generation mix shown as a stacked bar
(coal · gas · wind · solar · hydro · battery).

- **Setup / provisioning screen** — shown while in SoftAP mode: "Join WiFi `NEM-Buddy-XXXX` →
  your browser will open." Progress state as it connects.
- **Dashboard (Layout A — approved):** home-region **hero on top** (large price, demand,
  renewable %, trend arrow, mix bar) with a **calm ribbon of four region chips along the
  bottom** (name + price, negative prices tinted green). Tap the hero to drill in; tap a ribbon
  chip to promote that region to hero.
- **Drill-in (swipeable, 3 screens, page dots):**
  1. **Intraday history** — today's price curve with peak marker + a demand strip.
  2. **Generation mix** — ranked fuel breakdown with renewable % and total MW.
  3. **Interconnector flows** — home region's imports/exports to neighbours (direction + MW).
- **Alert takeover (Style 1 — full-bleed colour flood, approved):** the whole panel floods to
  the event colour (red spike / green negative) with the headline number and one context line.
  **Chime plays.** Tap to dismiss, or **auto-returns to the dashboard after ~30 s**
  (configurable).
- **Settings (on-screen touch):** home region (pick list), alert thresholds (sliders),
  quiet hours, brightness, mute, and "Change WiFi" (re-enters provisioning). WiFi *password*
  entry stays on the phone via the portal — no painful on-screen typing.

## 7. Error handling & resilience

- **WiFi drop:** auto-reconnect with exponential backoff; dashboard shows an offline indicator
  and dims; last-good data stays on screen marked stale with its age.
- **API failure / timeout:** keep last-good snapshot, mark stale, retry with backoff; never
  blank the screen on a single failed poll.
- **Malformed JSON:** parse defensively; a bad field falls back to "—" rather than crashing.
- **Time not yet synced:** hide the clock and any "updated Xm ago" until SNTP succeeds.
- **Task watchdog** on the fetch + UI loops; a wedged fetch task recovers without taking down
  rendering.

## 8. Testing

- **Host-side unit tests** for the two pure modules that carry the real logic:
  - `nem_client` parsing — feed recorded AEMO + OpenNEM JSON fixtures, assert normalized
    structs.
  - `alerts` engine — feed synthetic snapshots, assert correct events, debounce behaviour, and
    quiet-hours suppression.
- **Fixture replay on device** — a build flag that feeds recorded JSON instead of live HTTP, so
  the full UI + alert path (including takeover + chime) can be exercised deterministically.
- **Manual smoke** — provisioning flow (fresh NVS → SoftAP → connect), swipe navigation, region
  promotion, and a forced spike/negative fixture to confirm the takeover and chime.

## 9. Explicitly out of scope (v1 / YAGNI)

- Personal wholesale billing (Amber-style) integration.
- Historical data beyond "today" (no multi-day charts, no logging to flash).
- OTA updates, home-automation triggers, or a companion mobile app.
- Using the IMU (e.g. flip-to-mute) — hardware is present but unused in v1.

## 10. Open items to confirm during planning

- Exact Waveshare ESP-IDF driver versions / board-support commit to base bring-up on.
- Whether OpenNEM's per-region current endpoints cover generation mix + interconnectors at the
  cadence we want, or whether a second AEMO feed is needed for flows.
- Final chime sounds (short WAV assets vs synthesized tones).
