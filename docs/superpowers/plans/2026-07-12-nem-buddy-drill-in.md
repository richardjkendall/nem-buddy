# NEM Buddy — Drill-in Screens (Plan 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tapping the dashboard hero opens a swipeable 3-screen drill-in (intraday history / generation mix / interconnector flows) for the hero region; ribbon chips promote a region to hero.

**Architecture:** Proxy + host-tested `proxy_client` gain interconnector flows (`ni`/`ic`) and a parsed settlement epoch (`t`). `data_task` accumulates intraday history in PSRAM keyed off that epoch. `ui_dashboard` caches the latest snapshot/mix, tracks the current hero, and handles chip-tap promotion + hero-tap. A new `ui_drill` module renders the 3 tiles via `lv_tileview`.

**Tech Stack:** Python stdlib (proxy), C11 + cJSON + Unity (core), ESP-IDF v5.5 + LVGL v9 (firmware).

**Design spec:** `docs/superpowers/specs/2026-07-12-nem-buddy-drill-in-design.md`

## Global Constraints

- **Toolchain:** every firmware shell runs `source ~/esp/idf-env.sh` first. Build: `idf.py -C firmware build`. Agents do not flash (human flashes; agent may flash + serial-capture only in the UAT task with the human present).
- **Targets:** ESP-IDF **v5.5**, LVGL **v9** (use v9 API names — `lv_screen_active`, `lv_tileview_add_tile`, etc., never v8), target **esp32s3**. No on-device TLS.
- **Core dual-registration:** any new `core/src/*.c` goes in **both** `core/CMakeLists.txt` and `firmware/components/nem_core/CMakeLists.txt`. Core builds `-Wall -Wextra -Werror` (zero warnings). This plan adds no new core source (edits existing `proxy_client.c`), so no registration change.
- **LVGL rules:** all LVGL calls under `bsp_display_lock(-1)`/`bsp_display_unlock()`. `lv_color_hex()` only at runtime, never a file-scope initializer. Reuse `ui_theme.h` colors. Keep content inside the AMOLED safe-area (the dashboard uses `pad_all 20`).
- **Host test cycle:** `cmake --build core/build && ctest --test-dir core/build` (baseline currently 10/10).
- **Proxy:** Python **stdlib only** (no new deps); keep the payload compact.
- **Region enum order (regions.h):** NSW=0, QLD=1, SA=2, TAS=3, VIC=4. `NEM_REGION_COUNT`=5.

---

## File Structure

- `proxy/nem_proxy.py` — `fetch_aemo` captures `NETINTERCHANGE`+`INTERCONNECTORFLOWS`; `build_payload` emits `ni`/`ic` (Task 1)
- `core/src/proxy_client.c` — parse `ni`/`ic`/`t`; `core/test/test_proxy_client.c` + `core/test/fixtures/proxy_sample.json` (Task 2)
- `firmware/main/data_task.c` — PSRAM history accumulation + `nem_history_of()` accessor (Task 4)
- `firmware/main/ui_dashboard.{h,c}` — full-mix update signature, snapshot/mix cache, hero state + getters, chip promotion, hero-tap (Tasks 3, 7)
- `firmware/main/ui_drill.{h,c}` (new) — `lv_tileview` overlay + 3 tiles (Tasks 5, 6)
- `firmware/main/CMakeLists.txt` — add `ui_drill.c` (Task 5)

---

## Task 1: Proxy — emit interconnector flows (`ni`/`ic`)

**Files:**
- Modify: `proxy/nem_proxy.py` (`fetch_aemo`, `build_payload`)

**Interfaces:**
- Produces: payload regions gain `"ni": <float MW>` and `"ic": [[name, value], ...]` (name = AEMO interconnector name string, value = signed MW). Confirmed live schema: each AEMO row has `NETINTERCHANGE` (float) and `INTERCONNECTORFLOWS` (JSON **string** → list of `{"name","value","exportlimit","importlimit"}`).

- [ ] **Step 1: Extend `fetch_aemo` to capture net interchange + interconnector flows**

In `proxy/nem_proxy.py`, replace the `fetch_aemo` body's per-row `out[rid] = {...}` assignment so each region also carries `ni` and `ic`:

```python
def fetch_aemo():
    """-> (settlement_str, {region: {price, demand, ni, ic}})"""
    doc = json.loads(_get(AEMO_URL, {"User-Agent": "nem-buddy-proxy/1"}))
    out, settle = {}, None
    for row in doc.get("ELEC_NEM_SUMMARY", []):
        rid = row.get("REGIONID")
        if rid not in REGIONS:
            continue
        flows = []
        raw = row.get("INTERCONNECTORFLOWS")
        try:
            for f in (json.loads(raw) if isinstance(raw, str) else (raw or [])):
                name = f.get("name")
                if name is not None and f.get("value") is not None:
                    flows.append([str(name), round(float(f["value"]), 1)])
        except (ValueError, TypeError, AttributeError):
            flows = []
        out[rid] = {"price": float(row.get("PRICE", 0.0)),
                    "demand": float(row.get("TOTALDEMAND", 0.0)),
                    "ni": round(float(row.get("NETINTERCHANGE", 0.0)), 1),
                    "ic": flows}
        settle = settle or row.get("SETTLEMENTDATE")
    return settle, out
```

- [ ] **Step 2: Emit `ni`/`ic` in `build_payload`**

In `build_payload`, add the two fields to each region dict:

```python
        regions.append({
            "id": rid,
            "price": round(a.get("price", 0.0), 2),
            "demand": round(a.get("demand", 0.0)),
            "ni": a.get("ni", 0.0),
            "ic": a.get("ic", []),
            "ren": m.get("ren", 0.0),
            "fuel": m.get("fuel", {f: 0 for f in FUELS}),
        })
```

- [ ] **Step 3: Verify against live AEMO (no OE key needed — mix passed as None)**

Run:
```bash
cd proxy && python3 -c "import nem_proxy; s,a = nem_proxy.fetch_aemo(); p = nem_proxy.build_payload(s, a, None); import json; r = p['regions'][0]; print('t=', p['t']); print('id=', r['id'], 'ni=', r['ni']); print('ic=', json.dumps(r['ic']))"
```
Expected: prints a non-empty `t`, a numeric `ni`, and an `ic` list of `[name, value]` pairs (e.g. `[["T-V-MNSP1", 311.3], ["VIC1-NSW1", 528.8], ...]`).

- [ ] **Step 4: Commit**

```bash
git add proxy/nem_proxy.py
git commit -m "feat(proxy): emit per-region net interchange + interconnector flows"
```

---

## Task 2: Core — parse `ni`/`ic`/`t` in `proxy_client` (host-tested)

**Files:**
- Modify: `core/src/proxy_client.c`
- Modify: `core/test/fixtures/proxy_sample.json`
- Modify: `core/test/test_proxy_client.c`

**Interfaces:**
- Consumes: `nem_parse_iso8601` (`nem/timeutil.h`, already in `nem_core`).
- Produces: `nem_proxy_parse` now fills, per valid region: `settlement_epoch` (from top-level `"t"`), `net_interchange` (from `"ni"`), and `interconnectors[]` + `interconnector_count` (from `"ic"`), all existing `nem_region_snapshot_t` fields (`nem/snapshot.h`). Cap at `NEM_MAX_INTERCONNECTORS` (6); names truncated into `name[24]`.

- [ ] **Step 1: Update the fixture to carry `ni`/`ic` (and keep `t`)**

Replace `core/test/fixtures/proxy_sample.json` with:

```json
{"t":"2026-07-11T17:55:00","regions":[
{"id":"VIC1","price":19.24,"demand":7477,"ni":1064.7,"ic":[["T-V-MNSP1",311.3],["VIC1-NSW1",528.8],["V-SA",224.6]],"ren":0.445,"fuel":{"coal":4551,"gas":0,"hydro":40,"wind":2978,"solar":0,"battery":627,"other":0}},
{"id":"SA1","price":-4.67,"demand":1300,"ni":-224.6,"ic":[["V-SA",-224.6]],"ren":0.9,"fuel":{"coal":0,"gas":100,"hydro":0,"wind":900,"solar":0,"battery":50,"other":0}}
]}
```

- [ ] **Step 2: Write the failing test additions**

In `core/test/test_proxy_client.c`, add `#include "nem/timeutil.h"` near the top, add this test function before `main`, and add its `RUN_TEST` line:

```c
static void test_parses_interconnectors_and_epoch(void) {
    char *json = read_fixture("proxy_sample.json");
    nem_snapshot_t s; nem_region_mix_t m;
    TEST_ASSERT_TRUE(nem_proxy_parse(json, &s, &m));

    long long expect_epoch = 0;
    TEST_ASSERT_TRUE(nem_parse_iso8601("2026-07-11T17:55:00", &expect_epoch));

    const nem_region_snapshot_t *vic = &s.regions[NEM_REGION_VIC];
    TEST_ASSERT_EQUAL_INT64(expect_epoch, vic->settlement_epoch);
    TEST_ASSERT_EQUAL_DOUBLE(1064.7, vic->net_interchange);
    TEST_ASSERT_EQUAL_INT(3, vic->interconnector_count);
    TEST_ASSERT_EQUAL_STRING("T-V-MNSP1", vic->interconnectors[0].name);
    TEST_ASSERT_EQUAL_DOUBLE(311.3, vic->interconnectors[0].value);
    TEST_ASSERT_EQUAL_STRING("VIC1-NSW1", vic->interconnectors[1].name);
    TEST_ASSERT_EQUAL_DOUBLE(528.8, vic->interconnectors[1].value);

    const nem_region_snapshot_t *sa = &s.regions[NEM_REGION_SA];
    TEST_ASSERT_EQUAL_INT(1, sa->interconnector_count);
    TEST_ASSERT_EQUAL_DOUBLE(-224.6, sa->interconnectors[0].value);
    TEST_ASSERT_EQUAL_INT64(expect_epoch, sa->settlement_epoch);

    free(json);
}
```

Add to `main`:
```c
    RUN_TEST(test_parses_interconnectors_and_epoch);
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```bash
cmake --build core/build --target test_proxy_client && ctest --test-dir core/build -R test_proxy_client --output-on-failure
```
Expected: FAIL — `settlement_epoch`/`net_interchange`/`interconnector_count` are 0 (not yet parsed).

- [ ] **Step 4: Implement the parsing**

In `core/src/proxy_client.c`, add `#include "nem/timeutil.h"` after the existing includes. Then inside `nem_proxy_parse`, after `cJSON *root = cJSON_Parse(json);` and the null check, parse the shared settlement epoch once:

```c
    long long epoch = 0;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(t)) nem_parse_iso8601(t->valuestring, &epoch);
```

Then inside the `cJSON_ArrayForEach(el, regions)` loop, after `rs->demand_mw = num(el, "demand");`, add:

```c
        rs->settlement_epoch = epoch;
        rs->net_interchange = num(el, "ni");
        rs->interconnector_count = 0;
        const cJSON *ic = cJSON_GetObjectItemCaseSensitive(el, "ic");
        if (cJSON_IsArray(ic)) {
            const cJSON *pair = NULL;
            cJSON_ArrayForEach(pair, ic) {
                if (rs->interconnector_count >= NEM_MAX_INTERCONNECTORS) break;
                const cJSON *nm = cJSON_GetArrayItem(pair, 0);
                const cJSON *vl = cJSON_GetArrayItem(pair, 1);
                if (!cJSON_IsString(nm) || !cJSON_IsNumber(vl)) continue;
                nem_interconnector_flow_t *f = &rs->interconnectors[rs->interconnector_count++];
                snprintf(f->name, sizeof f->name, "%s", nm->valuestring);
                f->value = vl->valuedouble;
            }
        }
```

Add `#include <stdio.h>` (for `snprintf`) if not already present.

- [ ] **Step 5: Run test to verify it passes**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build
```
Expected: `100% tests passed` (test_proxy_client now 3 tests; suite 10 total unchanged count of executables).

- [ ] **Step 6: Commit**

```bash
git add core/src/proxy_client.c core/test/test_proxy_client.c core/test/fixtures/proxy_sample.json
git commit -m "feat(core): parse interconnector flows + settlement epoch in proxy_client"
```

---

## Task 3: Dashboard — full-mix update, snapshot/mix cache, hero state + promotion

**Files:**
- Modify: `firmware/main/ui_dashboard.h`
- Modify: `firmware/main/ui_dashboard.c`
- Modify: `firmware/main/data_task.c` (adapt to the new update signature)

**Interfaces:**
- Produces:
  - Changed: `void ui_dashboard_update(const nem_snapshot_t *snap, const nem_region_mix_t *mix)` — dashboard now takes the **full** region mix and picks the hero internally; caches both.
  - New: `nem_region_t ui_dashboard_hero_region(void)`, `const nem_snapshot_t *ui_dashboard_snapshot(void)`, `const nem_region_mix_t *ui_dashboard_mix(void)` — read under `bsp_display_lock`.
  - Behavior: ribbon chip tap promotes that region to hero and re-renders from the cache.
- Consumes: nothing new.

- [ ] **Step 1: Update the header**

Replace `firmware/main/ui_dashboard.h` with:

```c
#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H
#include "lvgl.h"
#include "nem/snapshot.h"
#include "nem/regions.h"
#include "nem/fuel.h"

void ui_dashboard_create(lv_obj_t *parent);

/* Update from the latest full snapshot + full region mix. Caches both and
 * renders the current hero region. Call inside bsp_display_lock(). */
void ui_dashboard_update(const nem_snapshot_t *snap, const nem_region_mix_t *mix);

/* Current hero region (home by default; changed by ribbon chip taps). */
nem_region_t ui_dashboard_hero_region(void);

/* Latest cached data (may be NULL before the first update). Read under lock. */
const nem_snapshot_t   *ui_dashboard_snapshot(void);
const nem_region_mix_t *ui_dashboard_mix(void);

#endif
```

- [ ] **Step 2: Rework `ui_dashboard.c` — cache, hero state, promotion, internal render**

In `firmware/main/ui_dashboard.c`:

(a) Extend the file-scope struct and add cache/hero state + a render helper. Replace the `static struct {...} d;` block with:

```c
static struct {
    lv_obj_t *region, *price, *unit, *demand_val, *renew_val;
    lv_obj_t *seg[NEM_FUEL_COUNT];
    lv_obj_t *chip[RIBBON_MAX];        /* chip container per ribbon slot */
    lv_obj_t *chip_name[RIBBON_MAX];
    lv_obj_t *chip_price[RIBBON_MAX];
    int chip_n;
    nem_region_t hero;
    nem_snapshot_t snap;
    nem_region_mix_t mix;
    bool have_data;
} d;
```

(b) Add `#include "nem/config.h"` is already present; add near the top after includes a forward declaration:

```c
static void render(void);
```

(c) In `ui_dashboard_create`, initialize hero to the default home region. Right after `lv_obj_set_style_pad_all(parent, 20, 0);` add:

```c
    nem_config_t cfg; nem_config_defaults(&cfg);
    d.hero = cfg.home_region;
    d.have_data = false;
```

(d) In the ribbon chip creation loop, store the chip + name objects and attach a click handler. Replace the loop body's tail (the part from `lv_obj_t *nm = mk(...)` through `d.chip_n++;`) with:

```c
        lv_obj_t *nm = mk(chip, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(nm, "—");
        lv_obj_t *pr = mk(chip, &lv_font_montserrat_20, NEM_C_WHITE, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(pr, "—");
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, chip_clicked_cb, LV_EVENT_CLICKED, NULL);
        d.chip[d.chip_n] = chip;
        d.chip_name[d.chip_n] = nm;
        d.chip_price[d.chip_n] = pr;
        d.chip_n++;
```

(e) Add the chip-click handler and a render helper. Add above `ui_dashboard_create`:

```c
/* Which region does ribbon slot `ci` show? The ribbon lists every region
 * except the hero, in enum order. */
static nem_region_t ribbon_region(int ci)
{
    int seen = 0;
    for (int r = 0; r < NEM_REGION_COUNT; r++) {
        if (r == d.hero) continue;
        if (seen == ci) return (nem_region_t)r;
        seen++;
    }
    return d.hero;
}

static void chip_clicked_cb(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_target(e);
    for (int i = 0; i < d.chip_n; i++) {
        if (d.chip[i] == chip) {
            d.hero = ribbon_region(i);
            render();
            break;
        }
    }
}
```

(f) Replace the body of `ui_dashboard_update` so it caches then renders, and move the actual drawing into `render()` (reading from the cache and `d.hero`):

```c
void ui_dashboard_update(const nem_snapshot_t *snap, const nem_region_mix_t *mix)
{
    d.snap = *snap;
    if (mix) d.mix = *mix;
    d.have_data = true;
    render();
}

static void render(void)
{
    if (!d.have_data) return;
    const nem_region_snapshot_t *h = &d.snap.regions[d.hero];
    const nem_fuel_mix_t *hm = &d.mix.regions[d.hero];

    lv_label_set_text(d.region, nem_region_name(d.hero));
    if (h->valid) {
        lv_label_set_text_fmt(d.price, "$%d", round_dollar(h->price));
        lv_obj_set_style_text_color(d.price, price_color(h->price), 0);
        lv_label_set_text_fmt(d.demand_val, "%d MW", (int)(h->demand_mw + 0.5));
    }
    if (hm->valid) {
        for (int i = 0; i < NEM_FUEL_COUNT; i++)
            lv_obj_set_flex_grow(d.seg[i], (int32_t)(hm->mw[i] + 0.5));
        lv_label_set_text_fmt(d.renew_val, "%d%%", (int)(hm->renewable_fraction * 100 + 0.5));
    }

    int ci = 0;
    for (int r = 0; r < NEM_REGION_COUNT && ci < d.chip_n; r++) {
        if (r == d.hero) continue;
        const nem_region_snapshot_t *rs = &d.snap.regions[r];
        lv_label_set_text(d.chip_name[ci], nem_region_name((nem_region_t)r));
        if (rs->valid) {
            lv_label_set_text_fmt(d.chip_price[ci], "$%d", round_dollar(rs->price));
            lv_obj_set_style_text_color(d.chip_price[ci], price_color(rs->price), 0);
        }
        ci++;
    }
}

nem_region_t ui_dashboard_hero_region(void) { return d.hero; }
const nem_snapshot_t   *ui_dashboard_snapshot(void) { return d.have_data ? &d.snap : NULL; }
const nem_region_mix_t *ui_dashboard_mix(void)      { return d.have_data ? &d.mix  : NULL; }
```

Remove the now-unused `lv_obj_set_user_data`/`lv_obj_get_user_data` chip wiring (superseded by `d.chip_name[]`). Ensure `chip_clicked_cb` is declared/defined before `ui_dashboard_create` uses it.

- [ ] **Step 3: Adapt `data_task.c` to the new signature**

In `firmware/main/data_task.c`, change the update call. Replace:

```c
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                const nem_fuel_mix_t *hm = &mix.regions[cfg.home_region];
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, hm, cfg.home_region);
                bsp_display_unlock();
                ESP_LOGI(TAG, "ok: %s $%.1f  demand %.0f  ren %.0f%%",
                         nem_region_name(cfg.home_region), h->price, h->demand_mw,
                         hm->renewable_fraction * 100.0);
```

with:

```c
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, &mix);
                bsp_display_unlock();
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                ESP_LOGI(TAG, "ok: %s $%.1f  demand %.0f",
                         nem_region_name(cfg.home_region), h->price, h->demand_mw);
```

- [ ] **Step 4: Build**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/ui_dashboard.h firmware/main/ui_dashboard.c firmware/main/data_task.c
git commit -m "feat(firmware): dashboard hero state + ribbon promotion; cache snapshot/mix"
```

---

## Task 4: Data task — accumulate intraday history in PSRAM

**Files:**
- Modify: `firmware/main/data_task.c`
- Create: `firmware/main/data_task.h` addition (accessor declaration)

**Interfaces:**
- Consumes: `nem_history_init`/`nem_history_add` (`nem/history.h`), `settlement_epoch` (Task 2).
- Produces: `const nem_region_history_t *nem_history_of(nem_region_t region)` — pointer into the task's PSRAM history (stable for process life; read under `bsp_display_lock`). Returns NULL before allocation.

- [ ] **Step 1: Declare the accessor**

In `firmware/main/data_task.h`, add (keep the existing `data_task_start`):

```c
#ifndef DATA_TASK_H
#define DATA_TASK_H
#include "nem/regions.h"
#include "nem/history.h"

void data_task_start(void);   /* polls + updates the UI (WiFi already up) */

/* Latest intraday history for a region (RAM-only, accumulated from boot).
 * NULL until the first poll allocates it. Read inside bsp_display_lock(). */
const nem_region_history_t *nem_history_of(nem_region_t region);

#endif
```

- [ ] **Step 2: Allocate + accumulate history in the fetch loop**

In `firmware/main/data_task.c`:

(a) add `#include "nem/history.h"` with the other `nem/` includes, and a file-scope pointer:

```c
static nem_history_t *s_hist;
```

(b) after the existing PSRAM fetch buffer allocation succeeds, allocate + init the history:

```c
    s_hist = heap_caps_malloc(sizeof(nem_history_t), MALLOC_CAP_SPIRAM);
    if (!s_hist) { ESP_LOGE(TAG, "no PSRAM history"); vTaskDelete(NULL); return; }
    nem_history_init(s_hist);
```

(c) inside the loop, right after a successful `nem_proxy_parse(...)` returns true (before/with the dashboard update, under no special ordering), add the history sample using the home region's settlement epoch:

```c
                long long epoch = snap.regions[cfg.home_region].settlement_epoch;
                if (epoch > 0) nem_history_add(s_hist, &snap, epoch);
```

(d) add the accessor at file scope:

```c
const nem_region_history_t *nem_history_of(nem_region_t region)
{
    if (!s_hist || region >= NEM_REGION_COUNT) return NULL;
    return &s_hist->regions[region];
}
```

- [ ] **Step 3: Build**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/data_task.c firmware/main/data_task.h
git commit -m "feat(firmware): accumulate intraday history in PSRAM (settlement-clock keyed)"
```

---

## Task 5: Drill-in — tileview scaffold + intraday history tile

**Files:**
- Create: `firmware/main/ui_drill.h`
- Create: `firmware/main/ui_drill.c`
- Modify: `firmware/main/CMakeLists.txt` (add `ui_drill.c`)

**Interfaces:**
- Consumes: `ui_dashboard_snapshot`/`ui_dashboard_mix` (Task 3), `nem_history_of` (Task 4), `ui_theme.h`.
- Produces: `void ui_drill_show(nem_region_t region)` (build + show the overlay for `region`), `void ui_drill_refresh(void)` (no-op if closed; else re-render the visible tile), `bool ui_drill_is_open(void)`. All called inside `bsp_display_lock()`.

- [ ] **Step 1: Header**

Create `firmware/main/ui_drill.h`:

```c
#ifndef UI_DRILL_H
#define UI_DRILL_H
#include <stdbool.h>
#include "nem/regions.h"

/* Open the drill-in overlay for `region` (3 swipeable tiles). Inside lock. */
void ui_drill_show(nem_region_t region);
/* Refresh the visible tile from the latest data; no-op if closed. Inside lock. */
void ui_drill_refresh(void);
bool ui_drill_is_open(void);

#endif
```

- [ ] **Step 2: Scaffold + history tile implementation**

Create `firmware/main/ui_drill.c`:

```c
#include "ui_drill.h"
#include "ui_theme.h"
#include "ui_dashboard.h"
#include "data_task.h"
#include "nem/snapshot.h"
#include "nem/fuel.h"
#include "nem/history.h"
#include "lvgl.h"
#include <stdio.h>

#define N_TILES 3

static struct {
    lv_obj_t *root;      /* full-screen overlay; NULL when closed */
    lv_obj_t *tv;        /* tileview */
    lv_obj_t *tile[N_TILES];
    lv_obj_t *dot[N_TILES];
    lv_obj_t *hist_chart;
    lv_chart_series_t *hist_ser;
    nem_region_t region;
    bool open;
} s;

static lv_color_t price_band(double p)
{
    if (p < 0)    return NEM_C_GREEN;
    if (p > 1000) return NEM_C_RED;
    if (p > 300)  return NEM_C_AMBER;
    return NEM_C_BLUE;
}

static int active_tile(void)
{
    lv_obj_t *act = lv_tileview_get_tile_act(s.tv);
    for (int i = 0; i < N_TILES; i++) if (s.tile[i] == act) return i;
    return 0;
}

static void update_dots(void)
{
    int a = active_tile();
    for (int i = 0; i < N_TILES; i++)
        lv_obj_set_style_bg_color(s.dot[i], i == a ? NEM_C_WHITE : NEM_C_MUTED, 0);
}

static void render_history(void)
{
    const nem_region_history_t *h = nem_history_of(s.region);
    if (!h) return;
    double lo = 1e12, hi = -1e12;
    int n = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
        if (!h->filled[i]) continue;
        double p = h->price[i];
        if (p < lo) lo = p;
        if (p > hi) hi = p;
        n++;
    }
    if (n == 0) { lv_chart_set_point_count(s.hist_chart, 1); return; }
    if (hi <= lo) hi = lo + 1;
    lv_chart_set_range(s.hist_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lo, (int32_t)hi);
    lv_chart_set_point_count(s.hist_chart, (uint32_t)n);
    int idx = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) {
        if (!h->filled[i]) continue;
        lv_chart_set_value_by_id(s.hist_chart, s.hist_ser, idx++, (int32_t)(h->price[i] + 0.5));
    }
    lv_obj_set_style_line_color(s.hist_chart, price_band(hi), LV_PART_ITEMS);
}

/* forward decls for the mix/interconnector tiles (Task 6) */
static void build_mix_tile(lv_obj_t *t);
static void build_ic_tile(lv_obj_t *t);
static void render_mix(void);
static void render_ic(void);

static lv_obj_t *tile_header(lv_obj_t *t, const char *title)
{
    lv_obj_t *lbl = lv_label_create(t);
    lv_obj_set_style_text_color(lbl, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(lbl, title);
    return lbl;
}

static void build_history_tile(lv_obj_t *t)
{
    tile_header(t, "TODAY  PRICE");
    lv_obj_t *c = lv_chart_create(t);
    lv_obj_set_size(c, 400, 300);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 10);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(c, 4, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);   /* hide point markers */
    s.hist_ser = lv_chart_add_series(c, NEM_C_BLUE, LV_CHART_AXIS_PRIMARY_Y);
    s.hist_chart = c;
}

static void close_drill(void)
{
    if (s.root) { lv_obj_del(s.root); s.root = NULL; }
    s.open = false;
}

static void root_clicked_cb(lv_event_t *e)
{
    (void)e;
    close_drill();
}

static void tv_changed_cb(lv_event_t *e)
{
    (void)e;
    update_dots();
    ui_drill_refresh();
}

void ui_drill_show(nem_region_t region)
{
    if (s.open) close_drill();
    s.region = region;

    s.root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s.root);
    lv_obj_set_style_bg_color(s.root, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s.root, 20, 0);
    lv_obj_add_flag(s.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s.root, root_clicked_cb, LV_EVENT_CLICKED, NULL);

    s.tv = lv_tileview_create(s.root);
    lv_obj_remove_style_all(s.tv);
    lv_obj_set_size(s.tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s.tv, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s.tv, tv_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s.tile[0] = lv_tileview_add_tile(s.tv, 0, 0, LV_DIR_RIGHT);
    s.tile[1] = lv_tileview_add_tile(s.tv, 1, 0, LV_DIR_HOR);
    s.tile[2] = lv_tileview_add_tile(s.tv, 2, 0, LV_DIR_LEFT);
    build_history_tile(s.tile[0]);
    build_mix_tile(s.tile[1]);
    build_ic_tile(s.tile[2]);

    /* page dots */
    lv_obj_t *dots = lv_obj_create(s.root);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, 80, 10);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < N_TILES; i++) {
        lv_obj_t *dot = lv_obj_create(dots);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_margin_left(dot, 4, 0);
        lv_obj_set_style_margin_right(dot, 4, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        s.dot[i] = dot;
    }

    s.open = true;
    update_dots();
    render_history();
    render_mix();
    render_ic();
}

void ui_drill_refresh(void)
{
    if (!s.open) return;
    switch (active_tile()) {
        case 0: render_history(); break;
        case 1: render_mix();     break;
        case 2: render_ic();      break;
    }
}

bool ui_drill_is_open(void) { return s.open; }
```

> Note: `build_mix_tile`/`build_ic_tile`/`render_mix`/`render_ic` are defined in Task 6. To keep this task's build green, add temporary stubs at the bottom of `ui_drill.c` now:
>
> ```c
> static void build_mix_tile(lv_obj_t *t) { tile_header(t, "GENERATION MIX"); }
> static void build_ic_tile(lv_obj_t *t)  { tile_header(t, "INTERCONNECTORS"); }
> static void render_mix(void) {}
> static void render_ic(void)  {}
> ```
> Task 6 replaces these stubs with the real implementations.

- [ ] **Step 3: Register the source**

In `firmware/main/CMakeLists.txt`, add `ui_drill.c` to `SRCS` (keep all existing entries and REQUIRES):

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "ui_setup.c" "ui_drill.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c" "net_manager.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server)
```

- [ ] **Step 4: Build**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -20
```
Expected: `Project build complete`. Fix any LVGL v9 API mismatches the compiler flags (e.g. exact `lv_chart_set_value_by_id` / `lv_tileview_get_tile_act` signatures) against the vendored LVGL headers under `firmware/managed_components/lvgl__lvgl/`.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/ui_drill.h firmware/main/ui_drill.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): drill-in tileview scaffold + intraday history tile"
```

---

## Task 6: Drill-in — generation mix + interconnector tiles

**Files:**
- Modify: `firmware/main/ui_drill.c`

**Interfaces:**
- Consumes: `ui_dashboard_mix`/`ui_dashboard_snapshot` (Task 3).
- Produces: replaces the Task-5 stubs with real `build_mix_tile`/`render_mix` and `build_ic_tile`/`render_ic`.

- [ ] **Step 1: Replace the mix + interconnector stubs**

Delete the four temporary stubs at the bottom of `ui_drill.c` and add these implementations (place them above `close_drill`, after `build_history_tile`). Add file-scope widget storage to the `s` struct first — extend the struct with:

```c
    lv_obj_t *mix_rows[NEM_FUEL_COUNT];   /* "Wind   2978 MW  44%" labels */
    lv_obj_t *mix_head;
    lv_obj_t *ic_head;
    lv_obj_t *ic_rows[NEM_MAX_INTERCONNECTORS];
```

Fuel labels + colors (add near the top of the file):

```c
static const char *const FUEL_NAME[NEM_FUEL_COUNT] = {
    "Coal", "Gas", "Hydro", "Wind", "Solar", "Battery", "Other"
};
static const uint32_t FUEL_HEX[NEM_FUEL_COUNT] = {
    0x5a5a5a, 0xe0a23b, 0x4a9eff, 0x37d67a, 0xffd23f, 0xb06bff, 0x8a8a92
};
```

Implementations:

```c
static void build_mix_tile(lv_obj_t *t)
{
    tile_header(t, "GENERATION MIX");
    s.mix_head = lv_label_create(t);
    lv_obj_set_style_text_color(s.mix_head, NEM_C_GREEN, 0);
    lv_obj_set_style_text_font(s.mix_head, &lv_font_montserrat_22, 0);
    lv_obj_align(s.mix_head, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s.mix_head, "—");
    for (int i = 0; i < NEM_FUEL_COUNT; i++) {
        lv_obj_t *l = lv_label_create(t);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(FUEL_HEX[i]), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, 70 + i * 30);
        lv_label_set_text(l, "");
        s.mix_rows[i] = l;
    }
}

static void render_mix(void)
{
    const nem_region_mix_t *m = ui_dashboard_mix();
    if (!m) return;
    const nem_fuel_mix_t *fm = &m->regions[s.region];
    if (!fm->valid || fm->total_mw <= 0) {
        lv_label_set_text(s.mix_head, "—");
        for (int i = 0; i < NEM_FUEL_COUNT; i++) lv_label_set_text(s.mix_rows[i], "");
        return;
    }
    lv_label_set_text_fmt(s.mix_head, "%d%% renewable   %d MW",
                          (int)(fm->renewable_fraction * 100 + 0.5), (int)(fm->total_mw + 0.5));
    /* rank by MW: simple selection over the fixed small array */
    int order[NEM_FUEL_COUNT];
    for (int i = 0; i < NEM_FUEL_COUNT; i++) order[i] = i;
    for (int i = 0; i < NEM_FUEL_COUNT; i++)
        for (int j = i + 1; j < NEM_FUEL_COUNT; j++)
            if (fm->mw[order[j]] > fm->mw[order[i]]) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
    for (int rank = 0; rank < NEM_FUEL_COUNT; rank++) {
        int f = order[rank];
        lv_obj_set_style_text_color(s.mix_rows[rank], lv_color_hex(FUEL_HEX[f]), 0);
        int pct = (int)(100.0 * fm->mw[f] / fm->total_mw + 0.5);
        lv_label_set_text_fmt(s.mix_rows[rank], "%-8s %5d MW  %2d%%", FUEL_NAME[f], (int)(fm->mw[f] + 0.5), pct);
    }
}

static void build_ic_tile(lv_obj_t *t)
{
    tile_header(t, "INTERCONNECTORS");
    s.ic_head = lv_label_create(t);
    lv_obj_set_style_text_color(s.ic_head, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(s.ic_head, &lv_font_montserrat_22, 0);
    lv_obj_align(s.ic_head, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s.ic_head, "—");
    for (int i = 0; i < NEM_MAX_INTERCONNECTORS; i++) {
        lv_obj_t *l = lv_label_create(t);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(l, NEM_C_WHITE, 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, 70 + i * 30);
        lv_label_set_text(l, "");
        s.ic_rows[i] = l;
    }
}

static void render_ic(void)
{
    const nem_snapshot_t *snap = ui_dashboard_snapshot();
    if (!snap) return;
    const nem_region_snapshot_t *rs = &snap->regions[s.region];
    double ni = rs->net_interchange;
    lv_label_set_text_fmt(s.ic_head, "Net: %s %d MW",
                          ni >= 0 ? "exporting" : "importing", (int)(ni < 0 ? -ni : ni) );
    lv_obj_set_style_text_color(s.ic_head, ni >= 0 ? NEM_C_AMBER : NEM_C_BLUE, 0);
    for (int i = 0; i < NEM_MAX_INTERCONNECTORS; i++) {
        if (i < rs->interconnector_count) {
            const nem_interconnector_flow_t *f = &rs->interconnectors[i];
            const char *arrow = f->value >= 0 ? "->" : "<-";
            lv_label_set_text_fmt(s.ic_rows[i], "%-10s %s %d MW",
                                  f->name, arrow, (int)(f->value < 0 ? -f->value : f->value));
            lv_obj_set_style_text_color(s.ic_rows[i], f->value >= 0 ? NEM_C_AMBER : NEM_C_BLUE, 0);
        } else {
            lv_label_set_text(s.ic_rows[i], "");
        }
    }
}
```

> `->`/`<-` are ASCII (the LVGL Montserrat font lacks the `→`/`←` glyphs — same lesson as Plan 3b: keep on-screen strings ASCII).

- [ ] **Step 2: Build**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
```
Expected: `Project build complete`.

- [ ] **Step 3: Commit**

```bash
git add firmware/main/ui_drill.c
git commit -m "feat(firmware): drill-in generation-mix + interconnector tiles"
```

---

## Task 7: Integrate hero-tap → drill-in; live refresh; on-board UAT

**Files:**
- Modify: `firmware/main/ui_dashboard.c` (hero click → `ui_drill_show`)
- Modify: `firmware/main/data_task.c` (call `ui_drill_refresh` each poll)

**Interfaces:**
- Consumes: `ui_drill_show`/`ui_drill_refresh` (Tasks 5–6), `ui_dashboard_hero_region` (Task 3).

- [ ] **Step 1: Make the hero panel open the drill-in**

In `firmware/main/ui_dashboard.c`, add `#include "ui_drill.h"` at the top. The hero price/region labels sit directly on `parent`; add an invisible clickable hit-area over the hero region and wire it. In `ui_dashboard_create`, right after the mix `bar` is created (before the ribbon), add a transparent clickable panel covering the hero area:

```c
    lv_obj_t *hero_hit = lv_obj_create(parent);
    lv_obj_remove_style_all(hero_hit);
    lv_obj_set_size(hero_hit, 440, 190);
    lv_obj_align(hero_hit, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(hero_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hero_hit, hero_clicked_cb, LV_EVENT_CLICKED, NULL);
```

And add the handler above `ui_dashboard_create`:

```c
static void hero_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_drill_show(ui_dashboard_hero_region());
}
```

> The hero hit-area is added before the ribbon so the ribbon chips render on top and keep their own taps; the hit-area sits over the hero labels/mix bar only.

- [ ] **Step 2: Refresh the open drill-in on each poll**

In `firmware/main/data_task.c`, add `#include "ui_drill.h"`, and inside the `bsp_display_lock` block where `ui_dashboard_update` is called, add a refresh right after it:

```c
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, &mix);
                ui_drill_refresh();
                bsp_display_unlock();
```

- [ ] **Step 3: Build + core suite**

Run:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware build 2>&1 | tail -15
cmake --build core/build && ctest --test-dir core/build
```
Expected: `Project build complete`; core `100% tests passed`.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/ui_dashboard.c firmware/main/data_task.c
git commit -m "feat(firmware): wire hero-tap to drill-in; refresh open drill-in each poll"
```

- [ ] **Step 5: On-board UAT (human-flashed, with the proxy running the new payload)**

Prereq: restart the proxy so it serves the new `ni`/`ic` payload (`NEM_OE_API_KEY=oe_xxx python3 proxy/nem_proxy.py --port 8080`). Human flashes:
```bash
source ~/esp/idf-env.sh && idf.py -C firmware -p /dev/cu.usbmodem21101 flash monitor
```
Confirm:
1. Dashboard shows live hero (VIC) as before.
2. Tap a ribbon chip → that region promotes to hero (hero panel + ribbon update instantly).
3. Tap the hero → drill-in opens on the history tile; swipe left/right through mix and interconnectors; page dots track the active tile.
4. History tile: price curve builds over time (sparse right after boot); mix tile: ranked fuels + renewable %/MW; interconnector tile: net figure + per-interconnector rows with direction + MW.
5. Tap anywhere → returns to the dashboard.
6. Promote a different region, drill in → tiles reflect that region.
7. Panel renders cleanly throughout (no white flush / `ESP_ERR_NO_MEM`).

---

## Self-Review

**Spec coverage:**
- Decision 1 (lv_tileview, 3 tiles) → Task 5. ✅
- Decision 2 (hero region + ribbon promotion) → Task 3 (promotion + getters) + Task 7 (hero-tap). ✅
- Decision 3 (tap-to-exit, swipe paging) → Task 5 (`root_clicked_cb`, tileview dirs). ✅
- Decision 4 (full interconnector breakdown) → Task 1 (proxy) + Task 2 (parser) + Task 6 (tile). ✅
- Decision 5 (history off settlement clock) → Task 2 (parse `t`) + Task 4 (`nem_history_add` with `settlement_epoch`). ✅
- Decision 6 (history RAM-only, PSRAM, sparse state) → Task 4 (PSRAM alloc) + Task 5 (`render_history` handles n==0). ✅
- Three tiles → Task 5 (history) + Task 6 (mix, interconnectors). ✅
- Live refresh while open → Task 7. ✅
- Host-tested parser → Task 2. ✅ On-board UAT → Task 7. ✅

**Placeholder scan:** Task 5 intentionally ships temporary stubs that Task 6 replaces — called out explicitly, not a hidden TODO. No other placeholders.

**Type consistency:** `ui_dashboard_update(snap, mix)` (2-arg, full mix) is defined in Task 3 and consumed by Task 4/7's data_task; `nem_history_of` returns `const nem_region_history_t *` (Task 4) consumed by Task 5. `ui_dashboard_hero_region`/`_snapshot`/`_mix` (Task 3) consumed by Tasks 5–7. `ui_drill_show`/`_refresh` (Task 5) consumed by Task 7. Snapshot fields `net_interchange`/`interconnectors[]`/`interconnector_count`/`settlement_epoch` are pre-existing in `snapshot.h`. Consistent.

**Note for the implementer:** the LVGL v9 chart/tileview calls (`lv_tileview_get_tile_act`, `lv_chart_set_value_by_id`, `lv_obj_set_style_size` on `LV_PART_INDICATOR`) should be checked against the vendored headers in `firmware/managed_components/lvgl__lvgl/` during Task 5's build; adjust to the exact v9 signatures if the compiler flags a mismatch. Keep all on-screen strings ASCII (the Montserrat font lacks arrow/ellipsis/em-dash glyphs — the Plan 3b lesson).
