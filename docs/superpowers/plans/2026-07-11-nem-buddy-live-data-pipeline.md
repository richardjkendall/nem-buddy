# NEM Buddy — Plan 3a: Live Data Pipeline Implementation Plan

> **For agentic workers:** Executes **inline with a human in the loop** — verification requires flashing the physical board, joining real WiFi, and observing live NEM data on screen. Build steps run in the local ESP-IDF toolchain; connectivity/flash steps need the human, board, and network. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Turn the static dashboard into a live one — the firmware joins WiFi, fetches AEMO (price/demand/interconnectors) and OpenElectricity (generation mix) over HTTPS, parses them with the `core/` library, and updates the Layout-A dashboard with real NEM data.

**Architecture:** `core/` is wrapped as an ESP-IDF component (its `src/*.c` compiled against ESP-IDF's bundled cJSON; the host-test `third_party/cJSON` is not used on-device). A single **data task** connects WiFi (STA), then loops: fetch AEMO every 60s and OpenElectricity every 5min, parse into `nem_snapshot_t` / `nem_region_mix_t`, accumulate `nem_history`, and push values into the dashboard under `bsp_display_lock()`. WiFi/API credentials live in a gitignored `secrets.h` (replaced by 3b's captive portal later). Home region is VIC (from `nem_config_defaults`) until 3b adds settings.

**Tech Stack:** ESP-IDF v5.5, LVGL v9, `esp_wifi`, `esp_http_client` + `esp_crt_bundle` (TLS), ESP-IDF `json` (cJSON), the `core/` library.

## Environment

- Build: `source ~/esp/idf-env.sh && idf.py -C firmware build`
- Flash+monitor: `idf.py -C firmware -p /dev/cu.usbmodem21101 flash monitor` (Ctrl-] to exit)
- The monitor shows `ESP_LOGI` output — used to confirm WiFi join, HTTP status, and parse results before trusting the screen.

## Global Constraints

- ESP-IDF **v5.5**, LVGL **v9**, target **esp32s3**.
- `core/` sources are the single source of truth — the ESP-IDF component references `core/src/*.c` and `core/include/`; do not copy/fork them into `firmware/`.
- On-device JSON uses ESP-IDF's `json` (cJSON) component; the core component `REQUIRES json`. The vendored `core/third_party/cJSON.*` is host-tests only and must not be compiled into the firmware.
- **Secrets:** `firmware/main/secrets.h` is **gitignored** and holds `NEM_WIFI_SSID`, `NEM_WIFI_PASSWORD`, `NEM_OE_API_KEY`. Commit only `secrets.h.example`. Never commit real credentials.
- All LVGL calls happen between `bsp_display_lock(-1)` / `bsp_display_unlock()`.
- HTTP response buffers are allocated from **PSRAM** (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`).
- Home region is `NEM_REGION_VIC`; the ribbon shows the other four in enum order.
- Colour palette per `ui_theme.h` (unchanged from Plan 2).

## File Structure

```
firmware/
  components/nem_core/CMakeLists.txt   # NEW: wraps core/src as an IDF component
  main/
    CMakeLists.txt                     # add new sources + REQUIRES
    idf_component.yml                  # (unchanged)
    secrets.h.example                  # NEW: template (committed)
    secrets.h                          # NEW: real creds (gitignored)
    wifi_sta.h / wifi_sta.c            # NEW: STA connect
    net_fetch.h / net_fetch.c          # NEW: HTTPS GET helper (cert bundle)
    data_task.h / data_task.c          # NEW: poll loop -> parse -> update UI
    ui_dashboard.h / ui_dashboard.c    # refactor: handles + ui_dashboard_update()
    ui_theme.h                         # (unchanged)
    main.c                             # start display, then start data task
```

---

## Task 1: Connectivity foundation (core component + WiFi STA)

**Files:**
- Create: `firmware/components/nem_core/CMakeLists.txt`
- Create: `firmware/main/secrets.h.example`, `firmware/main/secrets.h` (gitignored)
- Create: `firmware/main/wifi_sta.h`, `firmware/main/wifi_sta.c`
- Modify: root `.gitignore`, `firmware/main/CMakeLists.txt`, `firmware/main/main.c`

**Interfaces:**
- Produces: `esp_err_t wifi_sta_connect(void);` — inits NVS/netif/wifi, connects using `secrets.h`, blocks until got-IP (or returns error after retries).

- [ ] **Step 1: Gitignore secrets, add the example template**

Append to root `.gitignore`:
```
# Local credentials (never commit)
firmware/main/secrets.h
```
Create `firmware/main/secrets.h.example`:
```c
#ifndef SECRETS_H
#define SECRETS_H
/* Copy to secrets.h (gitignored) and fill in. */
#define NEM_WIFI_SSID     "your-wifi-ssid"
#define NEM_WIFI_PASSWORD "your-wifi-password"
#define NEM_OE_API_KEY    "your-openelectricity-api-key"  /* used in Task 3 */
#endif
```

- [ ] **Step 2: HUMAN — create `firmware/main/secrets.h`**

Copy the example to `firmware/main/secrets.h` and fill in real WiFi SSID/password (the OE key can wait until Task 3). Confirm the file exists before building.

- [ ] **Step 3: Wrap `core/` as an ESP-IDF component**

Create `firmware/components/nem_core/CMakeLists.txt`:
```cmake
# Wraps the host-tested core/ library as an ESP-IDF component.
# Sources stay the single source of truth under core/src.
set(CORE "${CMAKE_CURRENT_LIST_DIR}/../../../core")
idf_component_register(
    SRCS
        "${CORE}/src/regions.c"
        "${CORE}/src/config.c"
        "${CORE}/src/timeutil.c"
        "${CORE}/src/aemo_client.c"
        "${CORE}/src/history.c"
        "${CORE}/src/fuel.c"
        "${CORE}/src/oe_client.c"
        "${CORE}/src/alerts.c"
    INCLUDE_DIRS "${CORE}/include"
    REQUIRES json)
```

- [ ] **Step 4: Write `firmware/main/wifi_sta.h`**

```c
#ifndef WIFI_STA_H
#define WIFI_STA_H
#include "esp_err.h"
esp_err_t wifi_sta_connect(void);   /* blocks until connected or fails */
#endif
```

- [ ] **Step 5: Write `firmware/main/wifi_sta.c`**

```c
#include "wifi_sta.h"
#include "secrets.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retries;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < 20) { s_retries++; esp_wifi_connect(); ESP_LOGW(TAG, "retry %d", s_retries); }
        else xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_connect(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, NEM_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, NEM_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\"", NEM_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}
```
*Note:* if `strlcpy` is unavailable, use `strncpy` + explicit NUL. Validate at build.

- [ ] **Step 6: Register new sources + requires in `firmware/main/CMakeLists.txt`**

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif)
```

- [ ] **Step 7: Call it from `main.c` (temporary log-only connectivity check)**

Add to `app_main` after the dashboard is up (keep the existing bring-up + `ui_dashboard_create`):
```c
#include "wifi_sta.h"
...
    if (wifi_sta_connect() == ESP_OK) ESP_LOGI(TAG, "WiFi connected");
    else ESP_LOGE(TAG, "WiFi failed");
```

- [ ] **Step 8: Build**

`source ~/esp/idf-env.sh && idf.py -C firmware build` → expect the core component + wifi_sta compile and `Project build complete`. Fix component-path / `REQUIRES` issues against the real build output (e.g. if external `SRCS` paths need adjusting, switch to `EXTRA_COMPONENT_DIRS`).

- [ ] **Step 9: Commit (secrets.h excluded)**

```bash
git add firmware/components firmware/main/wifi_sta.c firmware/main/wifi_sta.h \
        firmware/main/secrets.h.example firmware/main/CMakeLists.txt firmware/main/main.c .gitignore
git status --short   # confirm secrets.h is NOT staged
git commit -m "feat(firmware): core as ESP-IDF component + WiFi STA connect"
```

- [ ] **Step 10: HUMAN CHECKPOINT — flash & confirm WiFi**

`idf.py -C firmware -p /dev/cu.usbmodem21101 flash monitor` — confirm the serial log shows `connecting to "<ssid>"` then `got IP a.b.c.d` and `WiFi connected`. Dashboard still shows the (dummy) Layout A. Report the log. Don't proceed until WiFi joins.

---

## Task 2: AEMO live (price, demand, ribbon)

Add the data task and make the dashboard updatable, then drive it from live AEMO data.

**Files:**
- Create: `firmware/main/net_fetch.h`, `firmware/main/net_fetch.c`
- Create: `firmware/main/data_task.h`, `firmware/main/data_task.c`
- Modify: `firmware/main/ui_dashboard.h`, `firmware/main/ui_dashboard.c` (handles + update)
- Modify: `firmware/main/main.c`, `firmware/main/CMakeLists.txt`

**Interfaces:**
- Produces: `esp_err_t nem_http_get(const char *url, const char *bearer, char *buf, size_t buf_sz, int *out_len);`
- Produces: `void data_task_start(void);` — spawns the poll task.
- Produces: `void ui_dashboard_update(const nem_snapshot_t *snap, nem_region_t home);` (mix params added in Task 3).

- [ ] **Step 1: Write `firmware/main/net_fetch.h`**

```c
#ifndef NET_FETCH_H
#define NET_FETCH_H
#include <stddef.h>
#include "esp_err.h"
/* HTTPS GET into caller buffer (NUL-terminated). bearer may be NULL.
 * Returns ESP_OK only on HTTP 200. */
esp_err_t nem_http_get(const char *url, const char *bearer, char *buf, size_t buf_sz, int *out_len);
#endif
```

- [ ] **Step 2: Write `firmware/main/net_fetch.c`**

```c
#include "net_fetch.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "fetch";

esp_err_t nem_http_get(const char *url, const char *bearer, char *buf, size_t buf_sz, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "nem-buddy/0.1",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    if (bearer) {
        char hdr[600];
        snprintf(hdr, sizeof(hdr), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
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
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "GET %s -> %d, %d bytes", url, status, total);
    return status == 200 ? ESP_OK : ESP_FAIL;
}
```

- [ ] **Step 3: Refactor `firmware/main/ui_dashboard.h`**

```c
#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H
#include "lvgl.h"
#include "nem/snapshot.h"
#include "nem/regions.h"

void ui_dashboard_create(lv_obj_t *parent);
/* Update live values. Call inside bsp_display_lock()/unlock(). */
void ui_dashboard_update(const nem_snapshot_t *snap, nem_region_t home);

#endif
```

- [ ] **Step 4: Refactor `firmware/main/ui_dashboard.c` to keep handles + update**

Replace the file with a version that (a) builds the same Layout A but stores widget handles in a file-static struct, and (b) adds `ui_dashboard_update`. Keep `ui_theme.h`, the safe-area insets, fonts, and the mix-bar/ribbon structure from Plan 2.

```c
#include "ui_dashboard.h"
#include "ui_theme.h"
#include "nem/config.h"
#include <stdio.h>

#define RIBBON_MAX (NEM_REGION_COUNT - 1)

static struct {
    lv_obj_t *region, *price, *unit, *demand_val, *renew_val;
    lv_obj_t *chip_price[RIBBON_MAX];
    nem_region_t chip_region[RIBBON_MAX];
    int chip_n;
} d;

static lv_color_t price_color(double p)
{
    if (p < 0)    return NEM_C_GREEN;   /* negative */
    if (p > 1000) return NEM_C_RED;     /* extreme spike */
    if (p > 300)  return NEM_C_AMBER;   /* spike */
    return NEM_C_WHITE;
}

static lv_obj_t *mk(lv_obj_t *p, const lv_font_t *f, lv_color_t col, lv_align_t a, int x, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, a, x, y);
    return l;
}

void ui_dashboard_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 20, 0);

    lv_obj_t *status = mk(parent, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(status, "NEM   LIVE");

    d.region = mk(parent, &lv_font_montserrat_26, NEM_C_BLUE, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_text(d.region, "—");
    d.price  = mk(parent, &lv_font_montserrat_48, NEM_C_WHITE, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_label_set_text(d.price, "—");
    d.unit   = mk(parent, &lv_font_montserrat_16, NEM_C_MUTED, LV_ALIGN_TOP_LEFT, 0, 122);
    lv_label_set_text(d.unit, "$/MWh");

    lv_obj_t *dl = mk(parent, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_TOP_RIGHT, 0, 34);
    lv_label_set_text(dl, "DEMAND");
    d.demand_val = mk(parent, &lv_font_montserrat_20, NEM_C_WHITE, LV_ALIGN_TOP_RIGHT, 0, 52);
    lv_label_set_text(d.demand_val, "—");
    lv_obj_t *rl = mk(parent, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_TOP_RIGHT, 0, 82);
    lv_label_set_text(rl, "RENEWABLES");
    d.renew_val = mk(parent, &lv_font_montserrat_20, NEM_C_GREEN, LV_ALIGN_TOP_RIGHT, 0, 100);
    lv_label_set_text(d.renew_val, "—");   /* live in Task 3 */

    /* mix bar placeholder (populated live in Task 3) */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 440, 12);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x141416), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    /* ribbon */
    lv_obj_t *ribbon = lv_obj_create(parent);
    lv_obj_remove_style_all(ribbon);
    lv_obj_set_size(ribbon, 440, 68);
    lv_obj_align(ribbon, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(ribbon, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ribbon, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ribbon, LV_OBJ_FLAG_SCROLLABLE);

    d.chip_n = 0;
    for (int r = 0; r < NEM_REGION_COUNT; r++) {
        /* home region filled in on first update; skip it here provisionally */
        lv_obj_t *chip = lv_obj_create(ribbon);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, 102, 64);
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x141416), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *nm = mk(chip, &lv_font_montserrat_14, NEM_C_MUTED, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(nm, "—");
        lv_obj_t *pr = mk(chip, &lv_font_montserrat_20, NEM_C_WHITE, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(pr, "—");
        /* store name label via user_data for update */
        lv_obj_set_user_data(chip, nm);
        d.chip_price[d.chip_n] = pr;
        d.chip_region[d.chip_n] = NEM_REGION_COUNT; /* assigned on first update */
        if (++d.chip_n == RIBBON_MAX) break;
    }
}

void ui_dashboard_update(const nem_snapshot_t *snap, nem_region_t home)
{
    const nem_region_snapshot_t *h = &snap->regions[home];
    lv_label_set_text(d.region, nem_region_name(home));
    if (h->valid) {
        lv_label_set_text_fmt(d.price, "$%d", (int)(h->price + (h->price < 0 ? -0.5 : 0.5)));
        lv_obj_set_style_text_color(d.price, price_color(h->price), 0);
        lv_label_set_text_fmt(d.demand_val, "%d MW", (int)(h->demand_mw + 0.5));
    }
    /* ribbon: every region except home, in enum order */
    int ci = 0;
    for (int r = 0; r < NEM_REGION_COUNT && ci < d.chip_n; r++) {
        if (r == home) continue;
        const nem_region_snapshot_t *rs = &snap->regions[r];
        lv_obj_t *pr = d.chip_price[ci];
        lv_obj_t *chip = lv_obj_get_parent(pr);
        lv_obj_t *nm = (lv_obj_t *)lv_obj_get_user_data(chip);
        lv_label_set_text(nm, nem_region_name((nem_region_t)r));
        if (rs->valid) {
            lv_label_set_text_fmt(pr, "$%d", (int)(rs->price + (rs->price < 0 ? -0.5 : 0.5)));
            lv_obj_set_style_text_color(pr, price_color(rs->price), 0);
        }
        d.chip_region[ci] = (nem_region_t)r;
        ci++;
    }
}
```

- [ ] **Step 5: Write `firmware/main/data_task.h`**

```c
#ifndef DATA_TASK_H
#define DATA_TASK_H
void data_task_start(void);   /* connects WiFi, then polls + updates the UI */
#endif
```

- [ ] **Step 6: Write `firmware/main/data_task.c` (AEMO only for now)**

```c
#include "data_task.h"
#include "wifi_sta.h"
#include "net_fetch.h"
#include "ui_dashboard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "nem/aemo_client.h"
#include "nem/history.h"
#include "nem/config.h"

static const char *TAG = "data";
#define AEMO_URL "https://visualisations.aemo.com.au/aemo/apps/api/report/ELEC_NEM_SUMMARY"
#define AEMO_BUF_SZ (32 * 1024)

static void data_task(void *arg)
{
    (void)arg;
    if (wifi_sta_connect() != ESP_OK) { ESP_LOGE(TAG, "no wifi; task exiting"); vTaskDelete(NULL); return; }

    nem_config_t cfg; nem_config_defaults(&cfg);
    static nem_history_t history;  /* ~24KB: static, NOT on the task stack */
    nem_history_init(&history);

    char *buf = heap_caps_malloc(AEMO_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no PSRAM buffer"); vTaskDelete(NULL); return; }

    for (;;) {
        int len = 0;
        if (nem_http_get(AEMO_URL, NULL, buf, AEMO_BUF_SZ, &len) == ESP_OK) {
            nem_snapshot_t snap;
            if (nem_aemo_parse_summary(buf, &snap)) {
                const nem_region_snapshot_t *h = &snap.regions[cfg.home_region];
                if (h->valid) nem_history_add(&history, &snap, h->settlement_epoch);
                bsp_display_lock(-1);
                ui_dashboard_update(&snap, cfg.home_region);
                bsp_display_unlock();
                ESP_LOGI(TAG, "AEMO ok: VIC $%.1f  demand %.0f", h->price, h->demand_mw);
            } else {
                ESP_LOGW(TAG, "AEMO parse failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

void data_task_start(void)
{
    /* Large stack: JSON parse + TLS. Buffers are PSRAM/static, but give headroom. */
    xTaskCreatePinnedToCore(data_task, "data", 8192, NULL, 5, NULL, tskNO_AFFINITY);
}
```

- [ ] **Step 7: Update `main.c` to start the data task (remove the Task-1 inline wifi test)**

```c
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui_dashboard.h"
#include "data_task.h"

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

    data_task_start();
}
```

- [ ] **Step 8: Update `firmware/main/CMakeLists.txt`**

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "wifi_sta.c" "net_fetch.c" "data_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls)
```

- [ ] **Step 9: Build**

`idf.py -C firmware build` → `Project build complete`. Fix any include/REQUIRES gaps (cert bundle lives in `esp-tls`/`mbedtls`; `esp_crt_bundle.h` needs `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`, on by default).

- [ ] **Step 10: Commit**

```bash
git add firmware/main
git commit -m "feat(firmware): live AEMO data driving price/demand/ribbon"
```

- [ ] **Step 11: HUMAN CHECKPOINT — flash & observe live prices**

`idf.py -C firmware -p /dev/cu.usbmodem21101 flash monitor`. Confirm: serial logs `AEMO ok: VIC $X demand Y`, and within ~a minute the screen's VIC price/demand and the four ribbon prices update to **real values** (cross-check against the log or the AEMO website), negative prices show green. Report a photo + log. Renewables/mix still show `—` (Task 3).

---

## Task 3: OpenElectricity live (renewables % + generation-mix bar)

**Files:**
- Modify: `firmware/main/ui_dashboard.h`, `firmware/main/ui_dashboard.c` (mix bar segments + renewables)
- Modify: `firmware/main/data_task.c` (OE fetch every 5 min)
- Requires: `NEM_OE_API_KEY` set in `secrets.h`

- [ ] **Step 1: HUMAN + VALIDATE — get the key and confirm the real OE response**

Add your `NEM_OE_API_KEY` to `secrets.h`. Then validate the endpoint and payload from the host so we set query params + buffer size from reality, not guesses:
```bash
curl -s -H "Authorization: Bearer <KEY>" \
  "https://api.openelectricity.org.au/v4/data/network/NEM?metrics=power&primary_grouping=network_region&secondary_grouping=fueltech" \
  -o /tmp/oe.json -w "HTTP %{http_code} size %{size_download}B\n"
python3 -c "import json;d=json.load(open('/tmp/oe.json'));print('keys',list(d));print('n_data',len(d.get('data',[])));import itertools;print('sample',d['data'][0] if d.get('data') else None)"
```
Record the exact URL that returns a small, current payload (add `&interval=5m` and a short date range if the default is large). Set `OE_BUF_SZ` to comfortably exceed the observed size (round up to the next 32 KB). If auth/shape differs from `nem_oe_parse_power`'s expectations, adjust the query — not the parser (the parser is host-tested).

- [ ] **Step 2: Extend `ui_dashboard.h` update signature**

```c
#include "nem/fuel.h"
void ui_dashboard_update(const nem_snapshot_t *snap, const nem_fuel_mix_t *mix, nem_region_t home);
```

- [ ] **Step 3: Add live mix bar + renewables to `ui_dashboard.c`**

Replace the placeholder `bar` with a flex row of `NEM_FUEL_COUNT` segments stored in `d.seg[]`, and in `ui_dashboard_update` set each segment's `flex_grow` from `mix->mw[i]` and update `renew_val`. Fuel colours:
```c
static const uint32_t k_fuel_hex[NEM_FUEL_COUNT] = {
    0x5a5a5a, /*coal*/ 0xe0a23b, /*gas*/ 0x4a9eff, /*hydro*/ 0x37d67a, /*wind*/
    0xffd23f, /*solar*/ 0xb06bff, /*battery*/ 0x8a8a92 /*other*/
};
```
In `create`, build the bar as a flex row and store segments:
```c
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < NEM_FUEL_COUNT; i++) {
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, LV_PCT(100));
        lv_obj_set_flex_grow(seg, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(k_fuel_hex[i]), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        d.seg[i] = seg;
    }
```
In `update` (guard `mix && mix->valid`):
```c
    if (mix && mix->valid) {
        for (int i = 0; i < NEM_FUEL_COUNT; i++)
            lv_obj_set_flex_grow(d.seg[i], (int32_t)(mix->mw[i] + 0.5));
        lv_label_set_text_fmt(d.renew_val, "%d%%", (int)(mix->renewable_fraction * 100 + 0.5));
    }
```
Add `lv_obj_t *seg[NEM_FUEL_COUNT];` to the `d` struct and `#include "nem/fuel.h"`.

- [ ] **Step 4: Add OE fetch to `data_task.c`**

Add the URL/key/buffer (size from Step 1), a 5-minute cadence counter, and parse into a `nem_region_mix_t`; pass the home region's `nem_fuel_mix_t` to `ui_dashboard_update`. Keep a last-good `nem_fuel_mix_t` so AEMO's 60s updates still pass the most recent mix:
```c
#include "nem/oe_client.h"
#include "secrets.h"
#define OE_URL "https://api.openelectricity.org.au/v4/data/network/NEM?metrics=power&primary_grouping=network_region&secondary_grouping=fueltech"
#define OE_BUF_SZ (/* from Step 1, e.g. */ 128 * 1024)
...
    static nem_fuel_mix_t last_mix;   /* zeroed => invalid until first OE fetch */
    char *oebuf = heap_caps_malloc(OE_BUF_SZ, MALLOC_CAP_SPIRAM);
    int ticks = 0;
    for (;;) {
        /* AEMO every loop (60s) */
        ... parse snap ...
        /* OE every 5 loops (~5 min), and on first iteration */
        if (oebuf && (ticks % 5 == 0)) {
            int olen = 0;
            if (nem_http_get(OE_URL, NEM_OE_API_KEY, oebuf, OE_BUF_SZ, &olen) == ESP_OK) {
                nem_region_mix_t mix;
                if (nem_oe_parse_power(oebuf, &mix)) last_mix = mix.regions[cfg.home_region];
            }
        }
        bsp_display_lock(-1);
        ui_dashboard_update(&snap, last_mix.valid ? &last_mix : NULL, cfg.home_region);
        bsp_display_unlock();
        ticks++;
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
```

- [ ] **Step 5: Build, commit, flash**

```bash
idf.py -C firmware build
git add firmware/main
git commit -m "feat(firmware): live OpenElectricity generation mix + renewables"
idf.py -C firmware -p /dev/cu.usbmodem21101 flash monitor
```

- [ ] **Step 6: HUMAN CHECKPOINT — fully live dashboard**

Confirm renewables % shows a real value and the mix bar reflects live fuel proportions (cross-check against openelectricity.org.au). Report a photo. Layout tweaks handled as follow-ups.

---

## Self-Review

**Spec coverage:** Delivers design-spec §4 (direct-to-API AEMO @60s + OpenElectricity @5min, self-accumulated history), §5 (`nem_client`/`store`/`history` wired on-device), §6 (live hero price/demand, ribbon, renewables, mix bar). Provisioning (§4/§6 setup) is Plan 3b; touch navigation/drill-in Plan 4; alerts/audio Plan 5.

**Placeholder scan:** No unresolved TBDs. Task-3 `OE_BUF_SZ`/query are explicitly validated against the real response before use (Step 1) — a deliberate empirical step, not a guess.

**Risk notes (validated at build/flash):** external-path `SRCS` in the core wrapper component (fallback: `EXTRA_COMPONENT_DIRS`); `strlcpy` availability; cert-bundle config; OE payload size/auth/query shape; task stack headroom for TLS+JSON (start 8192, raise if it stack-overflows). `nem_history_t` and buffers are static/PSRAM, never on the task stack (per Plan-1 review note).
