# NEM Buddy — Drill-in Screens (Plan 4) Design

**Date:** 2026-07-12
**Status:** Approved design (pre-implementation)
**Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16 (ESP-IDF v5.5 + LVGL v9)
**Parent design:** `docs/superpowers/specs/2026-07-10-nem-buddy-realtime-monitor-design.md` §6

## Problem / scope

Plan 4 delivers the **drill-in**: tapping the dashboard hero opens a swipeable set of
three detail screens for the current hero region. On-screen **Settings** (region /
thresholds / brightness / re-provision) was split out into a **later, separate plan** and
is out of scope here.

## Decisions

1. **Container = `lv_tileview`.** A drill-in overlay of 3 horizontal tiles (native swipe +
   snap-to-page), in order: (1) intraday history, (2) generation mix, (3) interconnector
   flows. We overlay our own page-dot row and a tap handler. Chosen over a manual screen
   stack (more code) and `lv_tabview` (unwanted tab chrome).
2. **Region = current hero, and Plan 4 adds ribbon promotion.** The Plan-3a dashboard has
   ribbon chips but no tap handlers yet. Plan 4 adds: (a) **chip-tap → promote** that
   region to hero (re-render the hero panel + ribbon from the cached snapshot), and (b)
   **hero-tap → drill** the current hero. The "current hero region" becomes tracked
   dashboard state (home region as the initial default), shared by the data task (renders
   the current hero each poll) and the drill-in (reads it at entry). Region is fixed for
   the life of a given drill-in session.
3. **Exit = tap anywhere.** Tiles are read-only, so a (non-swipe) tap unambiguously
   closes the drill-in and returns to the dashboard. Swipe left/right pages the tiles.
4. **Interconnector fidelity = full per-interconnector breakdown.** Each interconnector
   for the hero region: neighbour label, direction (import/export from MW sign), MW —
   plus the region's net interchange. (Not the net-only summary.)
5. **History time base = AEMO settlement clock, not device SNTP.** History buckets off the
   `settlement_epoch` carried in the proxy payload's `"t"` field (parsed via
   `nem_parse_iso8601`), so no device time-sync work is needed. Times are AEST-consistent
   (matches "today's" curve intent).
6. **History is RAM-only.** Accumulates from boot, fills through the day, resets on day
   rollover; no flash logging (per parent design §9). Fresh boot shows a partial curve +
   a "collecting…" note.

## Out of scope (later plans)

- On-screen Settings (home region, thresholds, quiet hours, brightness, mute, "Change
  WiFi" re-provision) — its own plan.
- Alerts takeover + chime — Plan 5/later.
- Multi-day history / flash logging, SNTP clock display.

## Architecture

**Dashboard promotion (`ui_dashboard`):**
- `ui_dashboard` gains a **current hero region** field (init = home region) with a getter.
- Each ribbon chip gets a click handler → set current hero to that region → re-render the
  hero panel + ribbon from the **cached latest snapshot/mix** (so promotion is instant, no
  wait for the next poll). `ui_dashboard` therefore caches the last `snapshot`/`mix`.
- `data_task` renders using the current hero (via `ui_dashboard_update` with the full
  snapshot; the dashboard picks hero vs ribbon internally) rather than the fixed
  `cfg.home_region`.
- The hero panel gets a click handler → `ui_drill_show(ui_dashboard_hero_region())`.

**Navigation & structure (`ui_drill`):**
- New `firmware/main/ui_drill.{h,c}` owns an `lv_tileview` overlay with 3 horizontal
  tiles + a page-dot row (bottom, inside the AMOLED safe-area inset) driven by the
  tileview's scroll/value-changed event.
- Entry: dashboard hero tap → `ui_drill_show(hero_region)`.
- Exit: tap (non-swipe) → close overlay → dashboard.
- Live updates: the 60 s data task keeps running; the visible tile refreshes on each new
  snapshot (price curve extends, MW numbers tick). All LVGL work under `bsp_display_lock`.

**Data plumbing:**

*(a) Interconnector flows (new data through the pipe):*
- Proxy (`nem_proxy.py`): `fetch_aemo` also extracts `NETINTERCHANGE` + the embedded
  `INTERCONNECTORFLOWS` JSON string per region; `build_payload` emits compact per-region
  `"ni": <mw>` and `"ic": [["VIC1-NSW1", -420], ["V-SA", 130]]` (name + signed MW; sign =
  direction). ~+400 bytes, well under the 8 KB device buffer.
- `proxy_client.c` (core, host-tested): parse `ni`/`ic` into the existing
  `nem_region_snapshot_t` fields (`net_interchange`, `interconnectors[]`,
  `interconnector_count`), and parse `"t"` → `settlement_epoch`. New fixture + assertions
  in `test_proxy_client.c`.

*(b) Intraday history accumulation (new on-device state):*
- `data_task.c`: allocate one `nem_history_t` (~24 KB) from PSRAM
  (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` — never on the task stack), `nem_history_init`
  once, then `nem_history_add(hist, &snap, snap.regions[home].settlement_epoch)` each poll.
  Accumulates all 5 regions so a promoted-then-drilled region has its curve. Expose a
  locked accessor for the latest snapshot/mix/history that `ui_drill` reads.

No new core structs, no new network dependency, no SNTP.

## The three tiles

Shared: compact header (hero region name + "updated Xm ago" / stale marker) + page dots;
tap exits.

- **Tile 1 — Intraday history:** today's price curve (`lv_chart` line over filled 5-min
  slots, colored by current price band from `ui_theme`) with a **peak marker** labeled
  with its `$`; a slimmer **demand strip** below. Sparse/empty → partial curve + a quiet
  "collecting today's data…" note.
- **Tile 2 — Generation mix:** **ranked** fuel breakdown (coal · gas · wind · solar ·
  hydro · battery) sorted by MW, each a labeled bar (MW + %) in the `ui_theme` fuel
  colors; headline renewable % + total MW. Degrades to "—" if mix unavailable.
- **Tile 3 — Interconnector flows:** headline net interchange ("Importing/Exporting N
  MW"); one row per interconnector — neighbour label, direction arrow (import/export from
  sign), MW, import/export tinted. AEMO codes prettified where cheap (e.g. `VIC1-NSW1` →
  "↔ NSW"), raw-ish label as fallback. Empty → just the net figure.

## Testing

- **Host-tested (core):** `proxy_client` interconnector (`ic`/`ni`) + `settlement_epoch`
  (`t`) parsing — new/extended fixture in `test_proxy_client.c` asserting
  `interconnectors[]`, `net_interchange`, `settlement_epoch`. History bucketing already
  host-tested (`test_history`).
- **Proxy:** run `nem_proxy.py`, confirm payload carries `ni`/`ic` per region.
- **On-device (human UAT):** tap hero → swipe the 3 tiles → tap to exit; promote a region
  then drill. Agent can flash + serial-confirm boot/data; human drives the touch/visual
  validation.

## File structure

- `proxy/nem_proxy.py` — extend `fetch_aemo` + `build_payload`.
- `core/src/proxy_client.c` + `core/include/nem/proxy_client.h` — parse `ni`/`ic`/`t`.
- `core/test/test_proxy_client.c` (+ fixture) — assertions.
- `firmware/main/ui_drill.{h,c}` (new) — tileview overlay, 3 tiles, page dots, tap-exit,
  `ui_drill_show(region)` / update-on-snapshot.
- `firmware/main/data_task.c` — PSRAM history accumulation + locked accessor; render the
  current hero (full snapshot to the dashboard) instead of fixed `cfg.home_region`.
- `firmware/main/ui_dashboard.c` — current-hero state + getter; ribbon chip-tap promotion
  (re-render from cached snapshot/mix); hero tap handler → `ui_drill_show(hero_region)`.
- Interconnector label prettify + import/export direction: UI-side (cosmetic; not core).
