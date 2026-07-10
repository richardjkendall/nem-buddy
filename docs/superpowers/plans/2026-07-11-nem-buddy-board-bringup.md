# NEM Buddy — Plan 2: Board Bring-up Implementation Plan

> **For agentic workers:** This plan executes **inline with a human in the loop**, not via automated subagents — verification requires flashing the physical Waveshare board and visually confirming the screen. Steps use checkbox (`- [ ]`) syntax for tracking. Build steps are automatable in the local ESP-IDF toolchain; flash/observe steps require the human + device.

**Goal:** Stand up an ESP-IDF firmware project that boots on the Waveshare ESP32-S3-Touch-AMOLED-2.16, brings up the AMOLED + capacitive touch via the vendor BSP, and renders our static Layout-A dashboard skeleton (dummy data) in LVGL.

**Architecture:** A new `firmware/` ESP-IDF v5.5 project based on Waveshare's known-good `02_lvgl_demo_v9` example, which vendors the board BSP component (`esp32_s3_touch_amoled_2_16` → CO5300 panel + CST9217 touch + LVGL adapter) and declares the deps `idf.py` resolves. We keep the BSP, `sdkconfig.defaults`, and `partitions.csv` unchanged; we replace only `main/`. UI is built inside the BSP's `bsp_display_lock()/unlock()` guard (LVGL runs in its own FreeRTOS task). No networking and no dependency on our `core/` library yet — that is Plan 3.

**Tech Stack:** ESP-IDF v5.5, LVGL v9, target `esp32s3`, Waveshare BSP, C11.

## Environment (already set up)

- Isolated ESP-IDF v5.5 at `~/esp/esp-idf-v5.5`, tools/venv under `~/esp/tools`, activated via `source ~/esp/idf-env.sh` (sets `IDF_TOOLS_PATH`, puts python3.12 ahead of system 3.8, sources `export.sh`).
- Verified: `idf.py` v5.5, python 3.12 venv, `xtensa-esp32s3-elf-gcc` 14.2.
- **Build (runs locally in this env):** `source ~/esp/idf-env.sh && idf.py -C firmware build`
- **Flash + monitor (human + board on USB):** `source ~/esp/idf-env.sh && idf.py -C firmware -p <PORT> flash monitor` where `<PORT>` is the board's USB serial device (e.g. `/dev/cu.usbmodemXXXX`). Exit monitor with `Ctrl-]`.

## Global Constraints

- ESP-IDF **v5.5**, LVGL **v9**, single target **esp32s3**. Do not upgrade/downgrade these.
- Base on Waveshare's `02_lvgl_demo_v9`; keep its vendored BSP component, `sdkconfig.defaults` (octal PSRAM @80MHz, 16MB flash QIO, 240MHz), and `partitions.csv` unchanged unless a build error demands otherwise (document any change).
- Display is **480×480, RGB565**. All LVGL UI construction happens between `bsp_display_lock(-1)` and `bsp_display_unlock()`.
- Colour palette (from the approved mockups, hex): background `0x000000`; normal/blue `0x4a9eff`; cheap/negative green `0x37d67a`; high/amber `0xe0a23b`; spike red `0xff3b2f`; muted text `0x8a8a92`. Generation-mix colours: coal `0x5a5a5a`, gas `0xe0a23b`, wind `0x37d67a`, solar `0xffd23f`, hydro `0x4a9eff`, battery `0xb06bff`.
- All dummy data in Plan 2 is clearly marked `/* TODO(plan3): live data */` so the wiring point is obvious.
- Git hygiene: gitignore `firmware/build/`, `firmware/managed_components/`, `firmware/sdkconfig`, `firmware/dependencies.lock` is COMMITTED. Commit `sdkconfig.defaults`, `partitions.csv`, source, and the vendored BSP component.

## File Structure

```
firmware/
  CMakeLists.txt                 # top-level project (from Waveshare example)
  sdkconfig.defaults             # PSRAM/flash/LVGL config (from example, unchanged)
  partitions.csv                 # (from example, unchanged)
  dependencies.lock              # committed after first resolve (reproducible deps)
  components/
    esp32_s3_touch_amoled_2_16/  # vendored Waveshare BSP (unchanged)
  main/
    CMakeLists.txt               # registers our sources
    idf_component.yml            # deps: local BSP + lvgl 9
    main.c                       # app_main: BSP init, then calls our UI
    ui_theme.h                   # shared palette + font constants
    ui_dashboard.c / ui_dashboard.h   # Layout-A screen (Task 2)
```

---

## Task 1: Firmware base + minimal bring-up (hello + touch proof)

Prove the toolchain, display, touch, and LVGL pipeline end-to-end with the smallest possible UI before building the real dashboard.

**Files:**
- Create: `firmware/` tree from Waveshare's `02_lvgl_demo_v9` (BSP, CMake, sdkconfig.defaults, partitions.csv)
- Rewrite: `firmware/main/main.c`
- Create: `firmware/main/ui_theme.h`
- Modify: root `.gitignore`

- [ ] **Step 1: Obtain Waveshare's example as the firmware base**

Clone the vendor repo shallow into a scratch dir and copy the example into `firmware/`:
```bash
git clone --depth 1 https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16.git /tmp/ws-amoled-216
cp -R "/tmp/ws-amoled-216/examples/ESP-IDF-v5.5/02_lvgl_demo_v9" firmware
ls firmware && ls firmware/components
```
Expected: `firmware/` contains `CMakeLists.txt`, `main/`, `components/esp32_s3_touch_amoled_2_16/`, `sdkconfig.defaults`, `partitions.csv`.

- [ ] **Step 2: Add gitignore entries**

Append to root `.gitignore`:
```
# ESP-IDF firmware build artifacts
firmware/build/
firmware/managed_components/
firmware/sdkconfig
firmware/sdkconfig.old
```

- [ ] **Step 3: Write the shared theme header `firmware/main/ui_theme.h`**

```c
#ifndef UI_THEME_H
#define UI_THEME_H
#include "lvgl.h"

/* Palette (approved mockups). */
#define NEM_C_BG        lv_color_hex(0x000000)
#define NEM_C_BLUE      lv_color_hex(0x4a9eff)   /* normal */
#define NEM_C_GREEN     lv_color_hex(0x37d67a)   /* cheap / negative */
#define NEM_C_AMBER     lv_color_hex(0xe0a23b)   /* high */
#define NEM_C_RED       lv_color_hex(0xff3b2f)   /* spike */
#define NEM_C_WHITE     lv_color_hex(0xe8e8ee)
#define NEM_C_MUTED     lv_color_hex(0x8a8a92)

/* Generation-mix fuel colours. */
#define NEM_C_COAL      lv_color_hex(0x5a5a5a)
#define NEM_C_GAS       lv_color_hex(0xe0a23b)
#define NEM_C_WIND      lv_color_hex(0x37d67a)
#define NEM_C_SOLAR     lv_color_hex(0xffd23f)
#define NEM_C_HYDRO     lv_color_hex(0x4a9eff)
#define NEM_C_BATTERY   lv_color_hex(0xb06bff)

#endif
```

- [ ] **Step 4: Rewrite `firmware/main/main.c` (minimal bring-up)**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui_theme.h"

static const char *TAG = "nem-buddy";
static int s_tap_count = 0;
static lv_obj_t *s_tap_label;

static void tap_cb(lv_event_t *e)
{
    (void)e;
    s_tap_count++;
    lv_label_set_text_fmt(s_tap_label, "taps: %d", s_tap_count);
    ESP_LOGI(TAG, "touch tap #%d", s_tap_count);
}

void app_main(void)
{
    ESP_LOGI(TAG, "NEM Buddy bring-up starting");
    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_brightness_set(80);

    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "NEM Buddy");
    lv_obj_set_style_text_color(title, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -28);

    s_tap_label = lv_label_create(scr);
    lv_label_set_text(s_tap_label, "taps: 0");
    lv_obj_set_style_text_color(s_tap_label, NEM_C_BLUE, 0);
    lv_obj_set_style_text_font(s_tap_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_tap_label, LV_ALIGN_CENTER, 0, 24);

    /* Screen-level click proves the touch pipeline end to end. */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);

    bsp_display_unlock();
    ESP_LOGI(TAG, "UI up; tap the screen to test touch");
}
```

*Note:* the exact BSP calls (`bsp_display_start`, `bsp_display_lock`, backlight/brightness) are validated against `firmware/components/esp32_s3_touch_amoled_2_16/include/bsp/*.h` at build time; if a signature differs (e.g. `bsp_display_lock` timeout units, or `bsp_display_start` needing a cfg struct), adapt to the header and note it. Mirror the vendor `main.c`'s own usage where in doubt.

- [ ] **Step 5: Ensure `main/CMakeLists.txt` registers our sources**

`firmware/main/CMakeLists.txt` should register `main.c` and include the current dir for `ui_theme.h`:
```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ".")
```
(Adapt if the example's version differs; keep any REQUIRES it already lists.)

- [ ] **Step 6: Set target and build (local toolchain)**

```bash
source ~/esp/idf-env.sh
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
```
Expected: dependency resolution downloads the BSP's transitive components (CO5300, CST9217, LVGL adapter, LVGL 9) into `firmware/managed_components/`, then a successful build ending in `Project build complete` and a `firmware/build/*.bin`. Fix any build errors against the real headers before proceeding.

- [ ] **Step 7: Commit the base + minimal bring-up**

```bash
git add firmware .gitignore
git commit -m "feat(firmware): ESP-IDF base + minimal AMOLED/touch bring-up"
```
(After Step 6 succeeds, `firmware/dependencies.lock` exists — include it in this commit for reproducible dep versions.)

- [ ] **Step 8: HUMAN CHECKPOINT — flash & observe**

Connect the board via USB-C, then:
```bash
source ~/esp/idf-env.sh
ls /dev/cu.*                      # find the port
idf.py -C firmware -p <PORT> flash monitor
```
**Confirm:** the AMOLED shows "NEM Buddy" (white) over "taps: 0" (blue) on a black background; tapping the screen increments the counter on-screen and logs `touch tap #N` in the serial monitor. Report success or the failure (photo + serial log). Do not proceed to Task 2 until display + touch are confirmed.

---

## Task 2: Static Layout-A dashboard skeleton (dummy data)

Build the approved Layout A — VIC hero on top, four-region ribbon along the bottom — with hardcoded values. This is the skeleton Plan 3 fills with live data.

**Files:**
- Create: `firmware/main/ui_dashboard.h`, `firmware/main/ui_dashboard.c`
- Modify: `firmware/main/main.c` (call `ui_dashboard_create()` instead of the hello UI)
- Modify: `firmware/main/CMakeLists.txt` (add `ui_dashboard.c`)

**Interfaces:**
- Produces: `void ui_dashboard_create(lv_obj_t *parent);` — builds the full dashboard into `parent` (the active screen), using internal dummy data.

- [ ] **Step 1: Write `firmware/main/ui_dashboard.h`**

```c
#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H
#include "lvgl.h"

/* Build the static Layout-A dashboard into `parent`. Dummy data for now. */
void ui_dashboard_create(lv_obj_t *parent);

#endif
```

- [ ] **Step 2: Write `firmware/main/ui_dashboard.c`**

```c
#include "ui_dashboard.h"
#include "ui_theme.h"

/* TODO(plan3): all values below are placeholders until the core data layer is wired in. */
typedef struct { const char *name; int price; lv_color_t color; } region_chip_t;

static const region_chip_t k_ribbon[] = {
    { "NSW", 118, NEM_C_WHITE },
    { "QLD",  76, NEM_C_WHITE },
    { "SA",  -18, NEM_C_GREEN },   /* negative -> green */
    { "TAS",  64, NEM_C_WHITE },
};

/* One generation-mix segment: fraction (0-100) + colour. */
typedef struct { int pct; lv_color_t color; } mix_seg_t;
static const mix_seg_t k_mix[] = {
    { 32, NEM_C_COAL }, { 18, NEM_C_GAS }, { 22, NEM_C_WIND },
    { 16, NEM_C_SOLAR }, { 12, NEM_C_HYDRO },
};

static void make_metric(lv_obj_t *parent, const char *label, const char *value,
                        lv_color_t value_color, lv_align_t align, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_align(l, align, 0, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, value_color, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_obj_align(v, align, 0, y + 16);
}

void ui_dashboard_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 14, 0);

    /* ---- Top status row ---- */
    lv_obj_t *status = lv_label_create(parent);
    lv_label_set_text(status, "NEM \xC2\xB7 LIVE");
    lv_obj_set_style_text_color(status, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_12, 0);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, 0, 0);

    /* ---- Hero: region + big price ---- */
    lv_obj_t *region = lv_label_create(parent);
    lv_label_set_text(region, "VICTORIA");
    lv_obj_set_style_text_color(region, NEM_C_BLUE, 0);
    lv_obj_set_style_text_font(region, &lv_font_montserrat_22, 0);
    lv_obj_align(region, LV_ALIGN_TOP_LEFT, 0, 26);

    lv_obj_t *price = lv_label_create(parent);
    lv_label_set_text(price, "$92");   /* TODO(plan3) */
    lv_obj_set_style_text_color(price, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(price, &lv_font_montserrat_48, 0);
    lv_obj_align(price, LV_ALIGN_TOP_LEFT, 0, 52);

    lv_obj_t *unit = lv_label_create(parent);
    lv_label_set_text(unit, "$/MWh  \xE2\x96\xBC 14%");
    lv_obj_set_style_text_color(unit, NEM_C_GREEN, 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_obj_align(unit, LV_ALIGN_TOP_LEFT, 0, 108);

    make_metric(parent, "DEMAND", "6,240 MW", NEM_C_WHITE, LV_ALIGN_TOP_RIGHT, 26);
    make_metric(parent, "RENEWABLES", "41%", NEM_C_GREEN, LV_ALIGN_TOP_RIGHT, 70);

    /* ---- Generation-mix stacked bar ---- */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 452, 10);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_radius(bar, 5, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < sizeof(k_mix) / sizeof(k_mix[0]); i++) {
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, LV_PCT(100));
        lv_obj_set_flex_grow(seg, k_mix[i].pct); /* proportional width */
        lv_obj_set_style_bg_color(seg, k_mix[i].color, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }

    /* ---- Bottom ribbon: 4 region chips ---- */
    lv_obj_t *ribbon = lv_obj_create(parent);
    lv_obj_remove_style_all(ribbon);
    lv_obj_set_size(ribbon, 452, 64);
    lv_obj_align(ribbon, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(ribbon, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ribbon, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ribbon, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(k_ribbon) / sizeof(k_ribbon[0]); i++) {
        lv_obj_t *chip = lv_obj_create(ribbon);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, 104, 60);
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *nm = lv_label_create(chip);
        lv_label_set_text(nm, k_ribbon[i].name);
        lv_obj_set_style_text_color(nm, NEM_C_MUTED, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);

        lv_obj_t *pr = lv_label_create(chip);
        lv_label_set_text_fmt(pr, "$%d", k_ribbon[i].price);
        lv_obj_set_style_text_color(pr, k_ribbon[i].color, 0);
        lv_obj_set_style_text_font(pr, &lv_font_montserrat_16, 0);
    }
}
```

*Note:* if any referenced Montserrat font (12/14/16/22/24/48) isn't enabled, add its `CONFIG_LV_FONT_MONTSERRAT_XX=y` to `sdkconfig.defaults` (the example already enables 12–26; **48 must be added**). Verify against the real LVGL config at build and adjust.

- [ ] **Step 3: Point `main.c` at the dashboard**

Replace the hello-UI body in `app_main` (the title/tap-label/click-handler block) with:
```c
#include "ui_dashboard.h"   /* add near the other includes */
...
    bsp_display_lock(-1);
    ui_dashboard_create(lv_screen_active());
    bsp_display_unlock();
```
Keep the `bsp_display_start()/backlight/brightness` lines and the logging. Remove the now-unused `tap_cb`, `s_tap_count`, `s_tap_label`.

- [ ] **Step 4: Add `ui_dashboard.c` to the build**

Update `firmware/main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c"
                       INCLUDE_DIRS ".")
```

- [ ] **Step 5: Add Montserrat 48 to `sdkconfig.defaults`**

Append `CONFIG_LV_FONT_MONTSERRAT_48=y` (the hero price uses it). Rebuild picks it up.

- [ ] **Step 6: Build (local toolchain)**

```bash
source ~/esp/idf-env.sh
idf.py -C firmware build
```
Expected: clean build, `Project build complete`. Fix any font/API errors against the real headers.

- [ ] **Step 7: Commit**

```bash
git add firmware
git commit -m "feat(firmware): static Layout-A dashboard skeleton (dummy data)"
```

- [ ] **Step 8: HUMAN CHECKPOINT — flash & observe**

```bash
source ~/esp/idf-env.sh
idf.py -C firmware -p <PORT> flash monitor
```
**Confirm against the approved mockup:** VIC hero with a large `$92`, `DEMAND 6,240 MW` and `RENEWABLES 41%` on the right, a thin multi-colour generation-mix bar, and a bottom ribbon of NSW/QLD/SA/TAS chips with SA's `-$18` in green. Report a photo. Layout tweaks (spacing/sizes) are expected and handled as follow-up edits to `ui_dashboard.c`.

---

## Self-Review

**Spec coverage:** Plan 2 delivers the board bring-up + static Layout A from the design spec §3 (ESP-IDF/LVGL stack), §6 (dashboard layout, palette, generation-mix bar, region ribbon). Networking, touch navigation, drill-in, settings, alerts, and audio are correctly out of scope (later plans). The `core/` data layer is intentionally not linked yet (Plan 3).

**Placeholder scan:** Dummy UI values are deliberate and tagged `TODO(plan3)`; they are the point of a skeleton, not gaps. No unresolved TBDs in build steps.

**Risk notes (hardware integration — validated empirically at build/flash, not assumable):**
- Exact BSP function signatures (`bsp_display_start`/`bsp_display_lock`/backlight) are confirmed against the vendored `components/esp32_s3_touch_amoled_2_16/include/bsp/*.h` when building; steps say to adapt to the header if they differ.
- Dependency resolution needs network on first build (downloads transitive components); `dependencies.lock` is committed after the first successful resolve for reproducibility.
- Montserrat 48 must be enabled for the hero price (Task 2 Step 5).
- Serial port name is discovered at flash time (`ls /dev/cu.*`), not assumed.
