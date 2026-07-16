# Device Status Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a full-screen device status overlay (battery, network, device identity, data health) toggled by the IO18 button, so the device can be diagnosed without a USB cable once it runs on battery.

**Architecture:** Pure logic (a Li-ion voltage→% curve and an "ago" formatter) lands in `core/` under Unity tests. A minimal AXP2101 driver reads the battery over the BSP's already-initialised shared I2C bus. A small task debounces GPIO18 and toggles an LVGL overlay built from four absolutely-positioned boxes in a 2×2 grid. The overlay refreshes from a 2s `lv_timer` that runs only while open — and because that timer lives in the LVGL task, which is also where touch I2C reads happen, every transaction on the shared bus stays on one thread.

**Tech Stack:** ESP-IDF v5.5, LVGL v9, esp32s3, Unity (core tests), CMake.

**Spec:** `docs/superpowers/specs/2026-07-17-device-status-screen-design.md`

## Global Constraints

- **Panel is 480×480** with a large corner radius. `BSP_LCD_H_RES`/`BSP_LCD_V_RES` in `firmware/components/esp32_s3_touch_amoled_2_16/include/bsp/display.h` are authoritative. The BSP README's "410×502" is the 2.06 board's panel and is wrong.
- **Safe area: inset 20 left / 40 right / 20 top / 20 bottom.** This panel clips ~20px more on the right than the left. Nothing meaningful may sit outside it.
- **Never use `lv_obj_set_flex_grow()` for proportional sizing on this board** — it does not size children proportionally here (a 0-value child rendered wide, which broke the dashboard fuel-mix bar). Use explicit pixel sizes or `LV_PCT`.
- **All LVGL calls from outside the LVGL task must be wrapped in `bsp_display_lock(-1)` / `bsp_display_unlock()`.**
- **`lv_color_hex()` is not a constant initializer** — store colours as hex `uint32_t` in any file-scope table.
- **Do not create a second I2C master bus** on GPIO14/15. The BSP owns it; use `bsp_i2c_get_handle()`.
- **`core/` is host-compiled with `-Wall -Wextra -Werror`** and must stay free of ESP-IDF/LVGL includes.
- **Toolchain:** every shell needs `source ~/esp/idf-env.sh` first. Build: `idf.py -C firmware build`. Flash: `idf.py -C firmware -p /dev/cu.usbmodem21101 flash`. Never run `idf.py monitor` (interactive) in an automated step.
- **Spec path correction:** the spec names `core/src/nem/battery.c`, but `core/src/` is **flat** (`src/fuel.c` with headers at `include/nem/fuel.h`). Follow the existing convention: `core/src/battery.c` + `core/include/nem/battery.h`.

---

## File Structure

| File | Responsibility |
|---|---|
| `core/include/nem/battery.h` + `core/src/battery.c` | Pure: Li-ion voltage→percentage curve |
| `core/test/test_battery.c` | Unity tests for the curve |
| `core/include/nem/timefmt.h` + `core/src/timefmt.c` | Pure: "12s ago" / "never" formatter |
| `core/test/test_timefmt.c` | Unity tests for the formatter |
| `firmware/main/axp2101.{c,h}` | Minimal AXP2101 driver over the BSP I2C bus |
| `firmware/main/buttons.{c,h}` | Debounced GPIO18 polling task |
| `firmware/main/ui_status.{c,h}` | The LVGL overlay (2×2 grid) + its 2s refresh timer |
| `firmware/main/wifi_ctrl.{c,h}` | *(modify)* add `wifi_ctrl_sta_info()` |
| `firmware/main/data_task.{c,h}` | *(modify)* track + expose fetch health |
| `firmware/main/main.c` | *(modify)* init the PMIC, start the button task |

Task order is risk-first: **Task 1 proves the PMIC answers before anything is built on top of it.** If it doesn't answer, stop and reassess — the battery half of the feature has no foundation, and the remaining tasks (button, overlay, network, health) still stand on their own.

---

### Task 1: Confirm the AXP2101 answers on the shared I2C bus

The PMIC has never been talked to on this board. Its address and chip ID are datasheet-derived, not observed. This task is a gate, not a feature.

**Files:**
- Create: `firmware/main/axp2101.h`, `firmware/main/axp2101.c`
- Modify: `firmware/main/CMakeLists.txt`, `firmware/main/main.c`

**Interfaces:**
- Consumes: `bsp_i2c_get_handle()` from the BSP.
- Produces: `void axp2101_scan_log(void);` — later tasks add `axp2101_init()` / `axp2101_read()` to this same header.

- [ ] **Step 1: Create the header**

```c
/* firmware/main/axp2101.h */
#ifndef AXP2101_H
#define AXP2101_H

/* Log every responding address on the BSP's shared I2C bus, then read and log
 * the AXP2101's chip ID. Diagnostic only — safe to call once at boot, after
 * the BSP has brought the I2C bus up. */
void axp2101_scan_log(void);

#endif
```

- [ ] **Step 2: Create the scanner**

```c
/* firmware/main/axp2101.c */
#include "axp2101.h"
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "axp";

/* Datasheet values — UNVERIFIED on this board until this task runs. */
#define AXP2101_ADDR      0x34
#define AXP2101_REG_CHIP  0x03
#define AXP2101_CHIP_ID   0x4A

void axp2101_scan_log(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) { ESP_LOGE(TAG, "no BSP I2C bus handle"); return; }

    char line[128];
    int n = snprintf(line, sizeof line, "I2C scan:");
    for (uint8_t a = 0x08; a < 0x78 && n > 0 && n < (int)sizeof line; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK)
            n += snprintf(line + n, sizeof line - n, " 0x%02X", a);
    }
    ESP_LOGI(TAG, "%s", line);

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed", AXP2101_ADDR);
        return;
    }
    uint8_t reg = AXP2101_REG_CHIP, id = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 200);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no reply at 0x%02X: %s", AXP2101_ADDR, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "chip id at 0x%02X = 0x%02X (expect 0x%02X) -> %s",
                 AXP2101_ADDR, id, AXP2101_CHIP_ID,
                 id == AXP2101_CHIP_ID ? "MATCH" : "MISMATCH");
    }
    i2c_master_bus_rm_device(dev);
}
```

- [ ] **Step 3: Register the source**

In `firmware/main/CMakeLists.txt`, add `"axp2101.c"` to `SRCS` and `esp_driver_i2c` to `REQUIRES`:

```cmake
idf_component_register(SRCS "main.c" "ui_dashboard.c" "ui_setup.c" "ui_drill.c" "net_fetch.c" "data_task.c" "net_creds.c" "wifi_ctrl.c" "captive_dns.c" "portal_http.c" "net_manager.c" "axp2101.c"
                       INCLUDE_DIRS "."
                       REQUIRES nem_core esp_wifi esp_event nvs_flash esp_netif
                                esp_http_client esp-tls lwip esp_http_server mbedtls
                                esp_driver_i2c)
```

- [ ] **Step 4: Call it once at boot**

In `firmware/main/main.c`, add `#include "axp2101.h"` and call it after the display is up (the BSP initialises I2C as part of display start):

```c
    bsp_display_lock(-1);
    ui_dashboard_create(lv_screen_active());
    bsp_display_unlock();

    axp2101_scan_log();          /* TEMPORARY: remove in Task 2 */

    net_manager_start();
```

- [ ] **Step 5: Build**

Run: `source ~/esp/idf-env.sh && idf.py -C firmware build`
Expected: `Project build complete.`

- [ ] **Step 6: Flash and capture the scan**

```bash
source ~/esp/idf-env.sh
idf.py -C firmware -p /dev/cu.usbmodem21101 flash
python3 - <<'PY' | grep -E "axp|I2C scan|chip id"
import serial, time, sys
s = serial.Serial('/dev/cu.usbmodem21101', 115200, timeout=0.2)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)   # reset AFTER opening
end = time.time() + 12
while time.time() < end:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode('utf-8','replace'))
s.close()
PY
```

Expected: an `I2C scan:` line listing several addresses (the touch controller, IMU, RTC and codec share this bus), including `0x34`, followed by `chip id at 0x34 = 0x4A ... MATCH`.

**Note:** the port must be opened *before* pulsing reset — that ordering is why an earlier probe attempt captured nothing.

- [ ] **Step 7: GATE — decide before continuing**

- **`MATCH`** → the PMIC is real and addressable. Continue to Task 2.
- **Chip ID reads but MISMATCH** → a different PMU variant. Stop; identify the part from the datasheet before writing `axp2101_read()` — the register map may differ.
- **No reply at 0x34, but the scan lists other addresses** → the bus works, the address is wrong. Try each unexplained address from the scan against the datasheet.
- **Scan lists nothing** → bus/BSP problem, not a PMIC problem. Stop and fix that first.

Record the actual scan output in the commit message — it is the only hard evidence we have of this board's bus.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/axp2101.c firmware/main/axp2101.h firmware/main/CMakeLists.txt firmware/main/main.c
git commit -m "feat(firmware): probe AXP2101 on the shared I2C bus

Paste the observed I2C scan + chip ID here."
```

---

### Task 2: AXP2101 driver — read battery state

**Files:**
- Modify: `firmware/main/axp2101.h`, `firmware/main/axp2101.c`, `firmware/main/main.c`

**Interfaces:**
- Consumes: the verified address/chip ID from Task 1.
- Produces:
  - `typedef struct { bool present; bool charging; uint8_t percent; uint16_t millivolts; } axp2101_state_t;`
  - `esp_err_t axp2101_init(void);`
  - `esp_err_t axp2101_read(axp2101_state_t *out);`

**Register map — datasheet-derived, confirm as you go.** Task 1 proved only the chip ID. If a value below reads back implausibly (e.g. percent > 100, or millivolts outside 2500–4500 with a cell attached), stop and check the datasheet rather than shipping a plausible-looking wrong number.

| Reg | Role | Notes |
|---|---|---|
| `0x00` | PMU status 1 | bit 3 = battery present |
| `0x01` | PMU status 2 | bits 6:5 == `0b01` → charging |
| `0x30` | ADC channel enable | bit 0 = battery voltage ADC |
| `0x34`/`0x35` | Battery voltage ADC | 14-bit, `((hi & 0x3F) << 8) | lo`, in mV |
| `0xA4` | Fuel gauge percentage | 0–100 |

- [ ] **Step 1: Replace the header**

```c
/* firmware/main/axp2101.h */
#ifndef AXP2101_H
#define AXP2101_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     present;      /* a cell is connected */
    bool     charging;     /* actively charging */
    uint8_t  percent;      /* 0..100 from the fuel gauge */
    uint16_t millivolts;   /* battery terminal voltage */
} axp2101_state_t;

/* Log every responding address on the BSP's shared I2C bus, then the chip ID. */
void axp2101_scan_log(void);

/* Probe the PMIC and enable the battery voltage ADC. Call once at boot, after
 * the BSP has brought I2C up. Returns ESP_OK only if the chip answered with the
 * expected ID; on failure the device is treated as absent and axp2101_read()
 * will fail cleanly rather than block. */
esp_err_t axp2101_init(void);

/* Fill `out` from the PMIC. Returns ESP_ERR_INVALID_STATE if init failed. */
esp_err_t axp2101_read(axp2101_state_t *out);

#endif
```

- [ ] **Step 2: Implement init + read**

Replace the body of `firmware/main/axp2101.c` below the existing `axp2101_scan_log()` (keep the scanner — it stays useful) and add:

```c
#define REG_STATUS1   0x00
#define REG_STATUS2   0x01
#define REG_ADC_EN    0x30
#define REG_VBAT_H    0x34
#define REG_PERCENT   0xA4

static i2c_master_dev_handle_t s_dev;
static bool s_ok;

static esp_err_t rd(uint8_t reg, uint8_t *val, size_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, n, 200);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(s_dev, b, sizeof b, 200);
}

esp_err_t axp2101_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) { ESP_LOGE(TAG, "no BSP I2C bus"); return ESP_ERR_INVALID_STATE; }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "PMIC not addressable; battery reports unavailable");
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t id = 0;
    if (rd(AXP2101_REG_CHIP, &id, 1) != ESP_OK || id != AXP2101_CHIP_ID) {
        ESP_LOGW(TAG, "PMIC id 0x%02X != 0x%02X; battery reports unavailable",
                 id, AXP2101_CHIP_ID);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t adc = 0;
    if (rd(REG_ADC_EN, &adc, 1) == ESP_OK)
        wr(REG_ADC_EN, adc | 0x01);      /* battery voltage ADC on */

    s_ok = true;
    ESP_LOGI(TAG, "AXP2101 ready");
    return ESP_OK;
}

esp_err_t axp2101_read(axp2101_state_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (axp2101_state_t){ 0 };
    if (!s_ok) return ESP_ERR_INVALID_STATE;

    uint8_t s1 = 0, s2 = 0, pct = 0, v[2] = { 0 };
    if (rd(REG_STATUS1, &s1, 1) != ESP_OK) return ESP_FAIL;
    if (rd(REG_STATUS2, &s2, 1) != ESP_OK) return ESP_FAIL;

    out->present  = (s1 & 0x08) != 0;
    out->charging = ((s2 >> 5) & 0x03) == 0x01;

    if (rd(REG_PERCENT, &pct, 1) == ESP_OK && pct <= 100) out->percent = pct;
    if (rd(REG_VBAT_H, v, 2) == ESP_OK)
        out->millivolts = (uint16_t)(((v[0] & 0x3F) << 8) | v[1]);

    return ESP_OK;
}
```

- [ ] **Step 3: Swap the temporary scan call for real init**

In `firmware/main/main.c`, replace the `axp2101_scan_log();` line added in Task 1:

```c
    if (axp2101_init() != ESP_OK)
        ESP_LOGW(TAG, "battery monitoring unavailable");
```

- [ ] **Step 4: Build**

Run: `source ~/esp/idf-env.sh && idf.py -C firmware build`
Expected: `Project build complete.`

- [ ] **Step 5: Verify on device**

Temporarily log one read at boot, after `axp2101_init()`:

```c
    axp2101_state_t bs;
    if (axp2101_read(&bs) == ESP_OK)
        ESP_LOGI(TAG, "batt: present=%d charging=%d %u%% %umV",
                 bs.present, bs.charging, bs.percent, bs.millivolts);
```

Flash and capture as in Task 1 Step 6, grepping for `batt:`.

Expected **with the battery installed**: `present=1`, a percent in 0–100, and millivolts in roughly 3000–4300. On USB with charging active, `charging=1`.
Expected **with no cell**: `present=0` — and `percent`/`millivolts` are then meaningless, which is exactly why the UI shows "No battery" rather than `0%`.

If percent reads `0` while voltage looks healthy, the fuel gauge likely needs its battery-detection/gauge enable bit set — consult the datasheet before proceeding, and note it in the commit.

Remove the temporary log once satisfied.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/axp2101.c firmware/main/axp2101.h firmware/main/main.c
git commit -m "feat(firmware): read battery state from the AXP2101

Record the observed present/charging/percent/millivolts here."
```

---

### Task 3: `core` — Li-ion voltage→percentage curve

Pure, host-tested. This is the fallback when the fuel gauge misbehaves on an uncharacterised cell, and the sanity check on its output.

**Files:**
- Create: `core/include/nem/battery.h`, `core/src/battery.c`, `core/test/test_battery.c`
- Modify: `core/CMakeLists.txt`

**Interfaces:**
- Produces: `uint8_t nem_batt_pct_from_mv(uint16_t mv);` — clamped 0..100.

- [ ] **Step 1: Write the failing test**

```c
/* core/test/test_battery.c */
#include "unity.h"
#include "nem/battery.h"

void setUp(void) {}
void tearDown(void) {}

static void test_clamps_rails(void) {
    TEST_ASSERT_EQUAL_UINT8(0,   nem_batt_pct_from_mv(0));
    TEST_ASSERT_EQUAL_UINT8(0,   nem_batt_pct_from_mv(3000));
    TEST_ASSERT_EQUAL_UINT8(0,   nem_batt_pct_from_mv(3300));
    TEST_ASSERT_EQUAL_UINT8(100, nem_batt_pct_from_mv(4200));
    TEST_ASSERT_EQUAL_UINT8(100, nem_batt_pct_from_mv(5000));
}

static void test_known_points(void) {
    TEST_ASSERT_EQUAL_UINT8(25,  nem_batt_pct_from_mv(3700));
    TEST_ASSERT_EQUAL_UINT8(55,  nem_batt_pct_from_mv(3800));
    TEST_ASSERT_EQUAL_UINT8(100, nem_batt_pct_from_mv(4200));
}

static void test_interpolates_between_points(void) {
    /* midway between 3700 (25%) and 3750 (40%) -> ~32% */
    uint8_t p = nem_batt_pct_from_mv(3725);
    TEST_ASSERT_TRUE(p > 25 && p < 40);
}

static void test_monotonic(void) {
    uint8_t prev = 0;
    for (uint16_t mv = 3300; mv <= 4200; mv += 10) {
        uint8_t p = nem_batt_pct_from_mv(mv);
        TEST_ASSERT_TRUE_MESSAGE(p >= prev, "curve must never decrease");
        prev = p;
    }
    TEST_ASSERT_EQUAL_UINT8(100, prev);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clamps_rails);
    RUN_TEST(test_known_points);
    RUN_TEST(test_interpolates_between_points);
    RUN_TEST(test_monotonic);
    return UNITY_END();
}
```

- [ ] **Step 2: Register the test and source**

In `core/CMakeLists.txt`, add `src/battery.c` to the `nem_core` source list and `nem_add_test(test_battery)` to the test list.

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S core -B core/build && cmake --build core/build --target test_battery`
Expected: FAIL — `nem/battery.h: No such file or directory`.

- [ ] **Step 4: Write the header**

```c
/* core/include/nem/battery.h */
#ifndef NEM_BATTERY_H
#define NEM_BATTERY_H

#include <stdint.h>

/* Estimate charge percentage (0..100) from a single-cell Li-ion terminal
 * voltage in millivolts, using a piecewise-linear discharge curve.
 *
 * This is a fallback and a sanity check for the AXP2101's own fuel gauge, which
 * depends on an internal battery model and can read oddly on a cell it has not
 * characterised. It is deliberately approximate: terminal voltage sags under
 * load, so a resting cell and a working one read differently at the same charge.
 * Clamped at both rails. */
uint8_t nem_batt_pct_from_mv(uint16_t mv);

#endif
```

- [ ] **Step 5: Write the implementation**

```c
/* core/src/battery.c */
#include "nem/battery.h"

/* Piecewise-linear approximation of a 3.7V Li-ion discharge curve. The curve is
 * flat through the middle and steep at the ends, which is why a lookup beats a
 * straight line. */
static const struct { uint16_t mv; uint8_t pct; } CURVE[] = {
    { 3300,   0 }, { 3600,  10 }, { 3700,  25 }, { 3750,  40 },
    { 3800,  55 }, { 3870,  70 }, { 3950,  85 }, { 4100,  95 },
    { 4200, 100 },
};
#define N (sizeof CURVE / sizeof CURVE[0])

uint8_t nem_batt_pct_from_mv(uint16_t mv)
{
    if (mv <= CURVE[0].mv)     return CURVE[0].pct;
    if (mv >= CURVE[N - 1].mv) return CURVE[N - 1].pct;

    for (size_t i = 1; i < N; i++) {
        if (mv <= CURVE[i].mv) {
            uint16_t span = CURVE[i].mv  - CURVE[i - 1].mv;
            uint8_t  rise = CURVE[i].pct - CURVE[i - 1].pct;
            uint16_t into = mv - CURVE[i - 1].mv;
            return (uint8_t)(CURVE[i - 1].pct + (into * rise + span / 2) / span);
        }
    }
    return CURVE[N - 1].pct;   /* unreachable */
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build core/build --target test_battery && ./core/build/test_battery`
Expected: `4 Tests 0 Failures 0 Ignored / OK`

- [ ] **Step 7: Commit**

```bash
git add core/include/nem/battery.h core/src/battery.c core/test/test_battery.c core/CMakeLists.txt
git commit -m "feat(core): add Li-ion voltage to percentage curve"
```

---

### Task 4: `core` — "ago" formatter

**Files:**
- Create: `core/include/nem/timefmt.h`, `core/src/timefmt.c`, `core/test/test_timefmt.c`
- Modify: `core/CMakeLists.txt`

**Interfaces:**
- Produces: `void nem_fmt_ago(long long secs, char *buf, size_t cap);` — `secs < 0` means "never".

- [ ] **Step 1: Write the failing test**

```c
/* core/test/test_timefmt.c */
#include "unity.h"
#include <string.h>
#include "nem/timefmt.h"

void setUp(void) {}
void tearDown(void) {}

static void expect(long long s, const char *want) {
    char b[24];
    nem_fmt_ago(s, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING(want, b);
}

static void test_never_for_negative(void) {
    expect(-1, "never");
    expect(-999, "never");
}

static void test_seconds(void) {
    expect(0,  "0s ago");
    expect(12, "12s ago");
    expect(59, "59s ago");
}

static void test_minutes(void) {
    expect(60,   "1m ago");
    expect(179,  "2m ago");
    expect(3599, "59m ago");
}

static void test_hours(void) {
    expect(3600,  "1h ago");
    expect(86399, "23h ago");
}

static void test_days(void) {
    expect(86400,  "1d ago");
    expect(259200, "3d ago");
}

static void test_truncates_rather_than_overflows(void) {
    char b[4];
    nem_fmt_ago(123456, b, sizeof b);
    TEST_ASSERT_TRUE(strlen(b) < sizeof b);   /* NUL-terminated, no overrun */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_never_for_negative);
    RUN_TEST(test_seconds);
    RUN_TEST(test_minutes);
    RUN_TEST(test_hours);
    RUN_TEST(test_days);
    RUN_TEST(test_truncates_rather_than_overflows);
    return UNITY_END();
}
```

- [ ] **Step 2: Register the test and source**

In `core/CMakeLists.txt`, add `src/timefmt.c` to `nem_core` and `nem_add_test(test_timefmt)`.

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S core -B core/build && cmake --build core/build --target test_timefmt`
Expected: FAIL — `nem/timefmt.h: No such file or directory`.

- [ ] **Step 4: Write the header**

```c
/* core/include/nem/timefmt.h */
#ifndef NEM_TIMEFMT_H
#define NEM_TIMEFMT_H

#include <stddef.h>

/* Render an elapsed duration as a short human string: "12s ago", "3m ago",
 * "5h ago", "2d ago". A negative `secs` means "it has not happened yet" and
 * renders as "never" — that is the caller's signal for "no successful fetch",
 * distinct from a genuine zero. Always NUL-terminates within `cap`. */
void nem_fmt_ago(long long secs, char *buf, size_t cap);

#endif
```

- [ ] **Step 5: Write the implementation**

```c
/* core/src/timefmt.c */
#include "nem/timefmt.h"
#include <stdio.h>

void nem_fmt_ago(long long secs, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;

    if (secs < 0)            snprintf(buf, cap, "never");
    else if (secs < 60)      snprintf(buf, cap, "%llds ago", secs);
    else if (secs < 3600)    snprintf(buf, cap, "%lldm ago", secs / 60);
    else if (secs < 86400)   snprintf(buf, cap, "%lldh ago", secs / 3600);
    else                     snprintf(buf, cap, "%lldd ago", secs / 86400);
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build core/build --target test_timefmt && ./core/build/test_timefmt`
Expected: `6 Tests 0 Failures 0 Ignored / OK`

- [ ] **Step 7: Run the whole core suite (no regressions)**

Run: `cmake --build core/build && ctest --test-dir core/build --output-on-failure`
Expected: all tests pass, including the pre-existing ones.

- [ ] **Step 8: Commit**

```bash
git add core/include/nem/timefmt.h core/src/timefmt.c core/test/test_timefmt.c core/CMakeLists.txt
git commit -m "feat(core): add elapsed-time 'ago' formatter"
```

---

### Task 5: Debounced IO18 button

**Files:**
- Create: `firmware/main/buttons.h`, `firmware/main/buttons.c`
- Modify: `firmware/main/CMakeLists.txt`

**Interfaces:**
- Produces: `typedef void (*button_cb_t)(void);` and `void buttons_start(button_cb_t on_press);`

**Hardware:** `IO18` = GPIO18, externally pulled up, shorted to GND when pressed. `PWR` is wired to the AXP2101's PWRON pin (not a GPIO) and `BOOT` is GPIO0 — **neither is used here**; leave both alone.

- [ ] **Step 1: Write the header**

```c
/* firmware/main/buttons.h */
#ifndef BUTTONS_H
#define BUTTONS_H

typedef void (*button_cb_t)(void);

/* Start a task polling the IO18 user button. `on_press` fires once per press,
 * on the falling edge, from the button task's context — so it MUST take
 * bsp_display_lock() before touching LVGL. */
void buttons_start(button_cb_t on_press);

#endif
```

- [ ] **Step 2: Write the implementation**

```c
/* firmware/main/buttons.c */
#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "btn";

#define BTN_IO18   GPIO_NUM_18
#define POLL_MS    20
#define STABLE_N   2      /* consecutive equal samples = 40ms debounce */

static button_cb_t s_cb;

static void button_task(void *arg)
{
    (void)arg;
    int stable = 1, candidate = 1, count = 0;

    for (;;) {
        int lvl = gpio_get_level(BTN_IO18);
        if (lvl != candidate) { candidate = lvl; count = 0; }
        else if (count < STABLE_N) { count++; }

        if (count >= STABLE_N && candidate != stable) {
            stable = candidate;
            if (stable == 0 && s_cb) {   /* falling edge = pressed */
                ESP_LOGI(TAG, "IO18 pressed");
                s_cb();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(button_cb_t on_press)
{
    s_cb = on_press;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BTN_IO18,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreatePinnedToCore(button_task, "btn", 3072, NULL, 4, NULL, tskNO_AFFINITY);
}
```

- [ ] **Step 3: Register the source**

Add `"buttons.c"` to `SRCS` in `firmware/main/CMakeLists.txt`.

- [ ] **Step 4: Build**

Run: `source ~/esp/idf-env.sh && idf.py -C firmware build`
Expected: `Project build complete.`

- [ ] **Step 5: Verify on device**

Temporarily wire a logging callback in `main.c`:

```c
static void btn_test_cb(void) { ESP_LOGI("main", "BUTTON PRESS"); }
/* ... in app_main: */
    buttons_start(btn_test_cb);
```

Flash, then run the capture from Task 1 Step 6 (grep for `BUTTON PRESS`) **and press IO18 several times during the 12-second window.**

Expected: one `IO18 pressed` + `BUTTON PRESS` per physical press — no repeats from a single press (that would mean the debounce is too short), and no spurious presses while idle.

Remove `btn_test_cb` once satisfied; Task 7 wires the real callback.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/buttons.c firmware/main/buttons.h firmware/main/CMakeLists.txt
git commit -m "feat(firmware): add debounced IO18 user button"
```

---

### Task 6: Status data accessors (network + data health)

The overlay needs facts that currently live nowhere. This task exposes them; no UI yet.

**Files:**
- Modify: `firmware/main/wifi_ctrl.h`, `firmware/main/wifi_ctrl.c`, `firmware/main/data_task.h`, `firmware/main/data_task.c`

**Interfaces:**
- Produces:
  - `typedef struct { bool connected; char ssid[33]; int8_t rssi; char ip[16]; } wifi_sta_info_t;`
  - `void wifi_ctrl_sta_info(wifi_sta_info_t *out);`
  - `typedef struct { long long uptime_s; long long last_ok_s; uint32_t consec_errors; } data_health_t;` — `last_ok_s` is the uptime at last success, or `-1` if never.
  - `void data_task_health(data_health_t *out);`

- [ ] **Step 1: Add the wifi accessor declaration**

Append to `firmware/main/wifi_ctrl.h` before `#endif`:

```c
typedef struct {
    bool   connected;
    char   ssid[33];
    int8_t rssi;
    char   ip[16];      /* dotted quad, empty when no IP */
} wifi_sta_info_t;

/* Snapshot the STA link. Always fills `out`; sets connected=false and leaves
 * ssid/ip empty when not associated. Safe to call from any task. */
void wifi_ctrl_sta_info(wifi_sta_info_t *out);
```

- [ ] **Step 2: Implement it**

Append to `firmware/main/wifi_ctrl.c`:

```c
void wifi_ctrl_sta_info(wifi_sta_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;   /* not associated */

    out->connected = true;
    out->rssi = ap.rssi;
    strlcpy(out->ssid, (const char *)ap.ssid, sizeof out->ssid);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0)
        snprintf(out->ip, sizeof out->ip, IPSTR, IP2STR(&ip.ip));
}
```

Ensure `wifi_ctrl.c` includes `<string.h>` and `<stdio.h>` (add if absent).

- [ ] **Step 3: Add the health declaration**

Append to `firmware/main/data_task.h` before `#endif`:

```c
#include <stdint.h>

typedef struct {
    long long uptime_s;        /* seconds since boot */
    long long last_ok_s;       /* uptime at last successful fetch, -1 = never */
    uint32_t  consec_errors;   /* consecutive failed/rejected polls */
} data_health_t;

/* Snapshot fetch health. Always fills `out`. Safe to call from any task. */
void data_task_health(data_health_t *out);
```

- [ ] **Step 4: Track health in the data task**

In `firmware/main/data_task.c`, add `#include "esp_timer.h"` and these file-scope statics beside `s_last_epoch`:

```c
static long long s_last_ok_s = -1;
static uint32_t  s_consec_errors;

static long long uptime_s(void) { return esp_timer_get_time() / 1000000; }
```

Then record outcomes at the three existing decision points inside the `for (;;)` loop:

- After `s_last_epoch = ep;` (a good, fresh payload) add:
  ```c
  s_last_ok_s = uptime_s();
  s_consec_errors = 0;
  ```
- In the stale/replayed branch, before `continue;` add:
  ```c
  s_consec_errors++;
  ```
- In the `nem_proxy_parse` failure `else` branch, after the existing `ESP_LOGW`, add:
  ```c
  s_consec_errors++;
  ```
- Add an `else` to the `nem_http_get(...) == ESP_OK` check so transport failures count too:
  ```c
  } else {
      s_consec_errors++;
      ESP_LOGW(TAG, "fetch failed");
  }
  ```

Append the accessor at the end of the file:

```c
void data_task_health(data_health_t *out)
{
    if (!out) return;
    out->uptime_s      = uptime_s();
    out->last_ok_s     = s_last_ok_s;
    out->consec_errors = s_consec_errors;
}
```

- [ ] **Step 5: Build**

Run: `source ~/esp/idf-env.sh && idf.py -C firmware build`
Expected: `Project build complete.`

- [ ] **Step 6: Verify on device**

The existing `data` log line already proves fetches succeed. Confirm the new counters compile in and read sensibly by temporarily logging them once in `data_task` after each poll:

```c
data_health_t h; data_task_health(&h);
ESP_LOGI(TAG, "health: up=%llds last_ok=%llds errs=%lu",
         h.uptime_s, h.last_ok_s, (unsigned long)h.consec_errors);
```

Flash and capture (Task 1 Step 6, grep `health:`).
Expected: `last_ok` moves to a real uptime after the first `ok:` line and `errs=0`. Remove the temporary log.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/wifi_ctrl.c firmware/main/wifi_ctrl.h firmware/main/data_task.c firmware/main/data_task.h
git commit -m "feat(firmware): expose STA link info and fetch health"
```

---

### Task 7: The status overlay

**Files:**
- Create: `firmware/main/ui_status.h`, `firmware/main/ui_status.c`
- Modify: `firmware/main/CMakeLists.txt`, `firmware/main/main.c`

**Interfaces:**
- Consumes: `axp2101_read()` (Task 2), `nem_batt_pct_from_mv()` (Task 3), `nem_fmt_ago()` (Task 4), `buttons_start()` (Task 5), `wifi_ctrl_sta_info()` / `data_task_health()` (Task 6).
- Produces: `void ui_status_toggle(void);` (call inside the display lock) and `bool ui_status_is_open(void);`

**Layout arithmetic** (safe area 20 left / 40 right / 20 top / 20 bottom on a 480×480 panel):

```
content width  = 480 - 20 - 40 = 420      grid x: 20 .. 440
content height = 480 - 20 - 20 = 440      grid y: 20 .. 460
header 24 + gap 9                          grid top = 53
box 205 × 199, gap 9   ->  205*2 + 9 = 419 <= 420   |   199*2 + 9 = 407 = 460 - 53
```

Built on `lv_layer_top()` so it sits above the drill-in without disturbing it, and positioned absolutely — **no flex-grow**, per the global constraints.

- [ ] **Step 1: Write the header**

```c
/* firmware/main/ui_status.h */
#ifndef UI_STATUS_H
#define UI_STATUS_H

#include <stdbool.h>

/* Toggle the device status overlay. Must be called inside
 * bsp_display_lock(-1) / bsp_display_unlock(). */
void ui_status_toggle(void);

bool ui_status_is_open(void);

#endif
```

- [ ] **Step 2: Write the overlay**

```c
/* firmware/main/ui_status.c */
#include "ui_status.h"
#include "ui_theme.h"
#include "axp2101.h"
#include "net_creds.h"
#include "wifi_ctrl.h"
#include "data_task.h"
#include "nem/battery.h"
#include "nem/timefmt.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_mac.h"

/* Safe area: this panel clips ~20px more on the right than the left. */
#define SX 20
#define SY 20
#define HDR_H 24
#define GAP 9
#define BOX_W 205
#define BOX_H 199
#define GY (SY + HDR_H + GAP)

static struct {
    lv_obj_t *root;
    lv_obj_t *batt_pct, *batt_bar, *batt_chg, *batt_mv;
    lv_obj_t *net_ssid, *net_rssi, *net_ip;
    lv_obj_t *h_last, *h_err, *h_up;
    lv_timer_t *timer;
    bool open;
} s;

static lv_obj_t *make_box(lv_obj_t *parent, int col, int row, const char *title)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BOX_W, BOX_H);
    lv_obj_set_pos(b, SX + col * (BOX_W + GAP), GY + row * (BOX_H + GAP));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0e0e12), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_pad_all(b, 13, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(t, 0, 0);
    return b;
}

/* A left-aligned "key   value" row at a given y inside a box. */
static lv_obj_t *make_row(lv_obj_t *box, int y, const char *key)
{
    lv_obj_t *k = lv_label_create(box);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(k, 0, y);

    lv_obj_t *v = lv_label_create(box);
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_color(v, NEM_C_WHITE, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(v, 110);
    lv_obj_set_pos(v, BOX_W - 26 - 110, y);
    return v;
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    if (!s.open) return;

    /* ---- battery ---- */
    axp2101_state_t b;
    char buf[32];
    if (axp2101_read(&b) == ESP_OK && b.present) {
        uint8_t pct = b.percent;
        /* Fuel gauge can read 0 on a cell it hasn't characterised; fall back to
         * the voltage curve rather than claim a flat battery. */
        if (pct == 0 && b.millivolts > 0) pct = nem_batt_pct_from_mv(b.millivolts);

        snprintf(buf, sizeof buf, "%u", pct);
        lv_label_set_text(s.batt_pct, buf);
        lv_obj_set_style_text_color(s.batt_pct, NEM_C_WHITE, 0);
        lv_obj_set_width(s.batt_bar, (int32_t)LV_PCT(pct > 100 ? 100 : pct));
        lv_obj_set_style_bg_color(s.batt_bar,
            pct <= 15 ? NEM_C_RED : (pct <= 35 ? NEM_C_AMBER : NEM_C_GREEN), 0);

        lv_label_set_text(s.batt_chg, b.charging ? "Charging" : "On battery");
        lv_obj_set_style_text_color(s.batt_chg, b.charging ? NEM_C_GREEN : NEM_C_MUTED, 0);
        snprintf(buf, sizeof buf, "%u.%02u V", b.millivolts / 1000, (b.millivolts % 1000) / 10);
        lv_label_set_text(s.batt_mv, buf);
    } else {
        lv_label_set_text(s.batt_pct, "n/a");
        lv_obj_set_style_text_color(s.batt_pct, NEM_C_MUTED, 0);
        lv_obj_set_width(s.batt_bar, 0);
        lv_label_set_text(s.batt_chg, "No battery");
        lv_obj_set_style_text_color(s.batt_chg, NEM_C_MUTED, 0);
        lv_label_set_text(s.batt_mv, "USB power");
    }

    /* ---- network ---- */
    wifi_sta_info_t w;
    wifi_ctrl_sta_info(&w);
    if (w.connected) {
        lv_label_set_text(s.net_ssid, w.ssid);
        lv_obj_set_style_text_color(s.net_ssid, NEM_C_WHITE, 0);
        snprintf(buf, sizeof buf, "%d dBm", w.rssi);
        lv_label_set_text(s.net_rssi, buf);
        lv_obj_set_style_text_color(s.net_rssi, w.rssi > -67 ? NEM_C_GREEN : NEM_C_AMBER, 0);
        lv_label_set_text(s.net_ip, w.ip[0] ? w.ip : "-");
    } else {
        lv_label_set_text(s.net_ssid, "disconnected");
        lv_obj_set_style_text_color(s.net_ssid, NEM_C_RED, 0);
        lv_label_set_text(s.net_rssi, "-");
        lv_obj_set_style_text_color(s.net_rssi, NEM_C_MUTED, 0);
        lv_label_set_text(s.net_ip, "-");
    }

    /* ---- data health ---- */
    data_health_t h;
    data_task_health(&h);
    nem_fmt_ago(h.last_ok_s < 0 ? -1 : h.uptime_s - h.last_ok_s, buf, sizeof buf);
    lv_label_set_text(s.h_last, buf);
    lv_obj_set_style_text_color(s.h_last, h.last_ok_s < 0 ? NEM_C_MUTED : NEM_C_GREEN, 0);

    snprintf(buf, sizeof buf, "%lu", (unsigned long)h.consec_errors);
    lv_label_set_text(s.h_err, buf);
    lv_obj_set_style_text_color(s.h_err, h.consec_errors ? NEM_C_RED : NEM_C_WHITE, 0);

    long long up = h.uptime_s;
    if (up < 3600) snprintf(buf, sizeof buf, "%llom %llos", up / 60, up % 60);
    else           snprintf(buf, sizeof buf, "%lloh %llom", up / 3600, (up % 3600) / 60);
    lv_label_set_text(s.h_up, buf);
}

static void build(void)
{
    /* layer_top so status sits above the drill-in overlay without touching it */
    s.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.root, NEM_C_BG, 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s.root, 0, 0);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s.root);
    lv_label_set_text(hdr, "DEVICE STATUS");
    lv_obj_set_style_text_color(hdr, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(hdr, SX, SY);

    /* ---- battery box ---- */
    lv_obj_t *bb = make_box(s.root, 0, 0, "BATTERY");
    s.batt_pct = lv_label_create(bb);
    lv_obj_set_style_text_font(s.batt_pct, &lv_font_montserrat_38, 0);
    lv_obj_set_style_text_color(s.batt_pct, NEM_C_WHITE, 0);
    lv_obj_set_pos(s.batt_pct, 0, 24);

    lv_obj_t *pc = lv_label_create(bb);
    lv_label_set_text(pc, "%");
    lv_obj_set_style_text_color(pc, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(pc, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(pc, 84, 44);

    lv_obj_t *track = lv_obj_create(bb);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, BOX_W - 26, 11);
    lv_obj_set_pos(track, 0, 80);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x1c1c20), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 6, 0);

    /* Explicit LV_PCT width from the data — never flex-grow on this board. */
    s.batt_bar = lv_obj_create(track);
    lv_obj_remove_style_all(s.batt_bar);
    lv_obj_set_size(s.batt_bar, 0, 11);
    lv_obj_set_pos(s.batt_bar, 0, 0);
    lv_obj_set_style_bg_color(s.batt_bar, NEM_C_GREEN, 0);
    lv_obj_set_style_bg_opa(s.batt_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s.batt_bar, 6, 0);

    s.batt_chg = lv_label_create(bb);
    lv_obj_set_style_text_font(s.batt_chg, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s.batt_chg, 0, 100);
    s.batt_mv = lv_label_create(bb);
    lv_obj_set_style_text_font(s.batt_mv, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s.batt_mv, NEM_C_MUTED, 0);
    lv_obj_set_pos(s.batt_mv, 0, 122);

    /* ---- network box ---- */
    lv_obj_t *nb = make_box(s.root, 1, 0, "NETWORK");
    s.net_ssid = lv_label_create(nb);
    lv_obj_set_style_text_font(s.net_ssid, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(s.net_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s.net_ssid, BOX_W - 26);
    lv_obj_set_pos(s.net_ssid, 0, 26);
    s.net_rssi = make_row(nb, 66, "Signal");
    s.net_ip   = make_row(nb, 92, "IP");

    /* ---- data health box ---- */
    lv_obj_t *hb = make_box(s.root, 0, 1, "DATA HEALTH");
    s.h_last = make_row(hb, 30, "Last fetch");
    s.h_err  = make_row(hb, 56, "Errors");
    s.h_up   = make_row(hb, 82, "Uptime");

    /* ---- device box (static values) ---- */
    lv_obj_t *db = make_box(s.root, 1, 1, "DEVICE");
    net_creds_t c;
    net_creds_load(&c);

    lv_obj_t *v_id = make_row(db, 30, "ID");
    lv_label_set_text(v_id, c.device_id[0] ? c.device_id : "-");

    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *v_fw = make_row(db, 56, "F/W");
    lv_label_set_text(v_fw, app ? app->version : "-");

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mbuf[16];
    snprintf(mbuf, sizeof mbuf, "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    lv_obj_t *v_mac = make_row(db, 82, "MAC");
    lv_label_set_text(v_mac, mbuf);

    /* Proxy host only — the full URL will not fit legibly in a quarter panel. */
    lv_obj_t *host = lv_label_create(db);
    const char *u = c.proxy_url;
    if (strncmp(u, "http://", 7) == 0) u += 7;
    else if (strncmp(u, "https://", 8) == 0) u += 8;
    char hbuf[48];
    strlcpy(hbuf, u[0] ? u : "-", sizeof hbuf);
    char *slash = strchr(hbuf, '/');
    if (slash) *slash = '\0';
    lv_label_set_text(host, hbuf);
    lv_obj_set_style_text_color(host, NEM_C_MUTED, 0);
    lv_obj_set_style_text_font(host, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(host, LV_LABEL_LONG_DOT);
    lv_obj_set_width(host, BOX_W - 26);
    lv_obj_set_pos(host, 0, 112);
}

void ui_status_toggle(void)
{
    if (s.open) {
        if (s.timer) { lv_timer_del(s.timer); s.timer = NULL; }
        if (s.root)  { lv_obj_del(s.root);    s.root  = NULL; }
        s.open = false;
        return;
    }
    build();
    s.open = true;
    refresh(NULL);                       /* paint immediately, don't wait 2s */
    s.timer = lv_timer_create(refresh, 2000, NULL);
}

bool ui_status_is_open(void) { return s.open; }
```

- [ ] **Step 3: Wire the button to the overlay**

In `firmware/main/main.c` add the includes and callback, and start the button task:

```c
#include "buttons.h"
#include "ui_status.h"

static void on_button(void)
{
    bsp_display_lock(-1);          /* button task must never touch LVGL unlocked */
    ui_status_toggle();
    bsp_display_unlock();
}
```

and in `app_main`, after `axp2101_init()`:

```c
    buttons_start(on_button);
```

- [ ] **Step 4: Register the source**

Add `"ui_status.c"` to `SRCS` in `firmware/main/CMakeLists.txt`.

- [ ] **Step 5: Build**

Run: `source ~/esp/idf-env.sh && idf.py -C firmware build`
Expected: `Project build complete.`

If a `lv_font_montserrat_38` link error appears, enable that size in `firmware/sdkconfig.defaults` (`CONFIG_LV_FONT_MONTSERRAT_38=y`) and rebuild — LVGL only compiles in the font sizes that are configured. Same for any other size used here (12/14/16/18).

- [ ] **Step 6: Flash and verify**

```bash
source ~/esp/idf-env.sh
idf.py -C firmware -p /dev/cu.usbmodem21101 flash
```

Then, by hand:
1. Press IO18 → the status overlay appears.
2. Press IO18 again → it disappears, dashboard intact.
3. Tap a region chip to open the drill-in, then press IO18 → status appears **above** the drill; press again → back to the drill, not the dashboard.
4. Leave it open ~10s → uptime and "last fetch" advance (proves the 2s timer runs).

- [ ] **Step 7: Commit**

```bash
git add firmware/main/ui_status.c firmware/main/ui_status.h firmware/main/CMakeLists.txt firmware/main/main.c
git commit -m "feat(firmware): add IO18-toggled device status overlay"
```

---

### Task 8: On-glass polish and UAT

The corner radius is a mockup estimate (~96px). This task checks it against real glass and fixes what the mockup could not tell us.

**Files:**
- Modify: `firmware/main/ui_status.c` (only if the check finds problems)

- [ ] **Step 1: Check the safe area on the device**

With the overlay open, look at all four box corners and the header.

Expected: nothing clipped by the rounded corners; the right-hand column is not cut off. If the outer corners of the top/bottom boxes are shaved, reduce `BOX_W`/`BOX_H` or increase the corner radius of the boxes — **do not** widen past the 20/40 safe inset.

- [ ] **Step 2: Verify battery readings against reality (human UAT — cannot be automated)**

1. Running on **USB with the battery connected** → "Charging", percent climbs slowly.
2. **Unplug USB** → "On battery", percent holds then falls; voltage falls with it.
3. Sanity-check percent against voltage: ~4.2V should read near 100%, ~3.7V near 25%. If the fuel gauge and the voltage curve disagree wildly, the gauge needs configuring — the voltage is the honest number, and that disagreement is exactly what showing both was for.
4. **Disconnect the battery, run on USB only** → "No battery / n/a", not "0%".

- [ ] **Step 3: Verify degraded states**

1. Turn off the `bhop` network (or move out of range) → SSID shows red "disconnected"; errors climb; identity and uptime keep rendering.
2. Press IO18 within the first minute of a cold boot, before the first fetch → "Last fetch: never" in muted grey, not "0s ago".

- [ ] **Step 4: Commit any fixes**

```bash
git add firmware/main/ui_status.c
git commit -m "fix(firmware): adjust status overlay layout to real glass"
```

- [ ] **Step 5: Update the spec's open risks**

Both risks in the spec — "the PMIC has never been talked to" and "the corner radius is a mockup estimate" — are now resolved or refuted. Replace that section with what was actually observed (the real I2C scan, the confirmed register behaviour, the final radius), so the spec stops warning about settled questions.

```bash
git add docs/superpowers/specs/2026-07-17-device-status-screen-design.md
git commit -m "docs: record verified PMIC and layout findings"
```

---

## Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| IO18 button, debounced | 5 |
| Minimal in-repo AXP2101 driver | 1, 2 |
| Share BSP I2C bus, no second master | 1, 2 |
| All I2C on the LVGL thread | 7 (`lv_timer`), 5 (button task does no I2C) |
| Toggle, stays open | 7 |
| Pure overlay above the drill-in | 7 (`lv_layer_top()`), verified in 7 Step 6 |
| Voltage alongside percentage | 7 |
| 2×2 grid layout | 7 |
| Truncated MAC + proxy host | 7 |
| Battery / Network / Identity / Health sections | 7, sourced by 2 and 6 |
| Voltage→% curve in `core/`, tested | 3 |
| `nem_fmt_ago`, tested | 4 |
| Degraded states | 7 (branches), 8 (verification) |
| Safe-area inset, no flex-grow | 7, checked in 8 |
| First step = I2C scan | 1 |

No gaps.

**Type consistency:** `axp2101_state_t` (Task 2) is consumed unchanged in Task 7. `wifi_sta_info_t` and `data_health_t` (Task 6) match their use in Task 7. `nem_batt_pct_from_mv(uint16_t) -> uint8_t` and `nem_fmt_ago(long long, char*, size_t)` are consistent between Tasks 3/4 and 7. The spec's `core/src/nem/battery.c` path is corrected to `core/src/battery.c` in Global Constraints and used consistently.

**Deviations from spec, recorded deliberately:**
1. **`core/src/nem/` → `core/src/`** — the spec invented a subdirectory the repo doesn't use.
2. **`lv_layer_top()` rather than `lv_screen_active()`** — the drill-in uses `lv_screen_active()`, but status must layer *above* it; `layer_top` guarantees that regardless of sibling order.
3. **Fuel-gauge fallback wired up in Task 7** — the spec called the `core/` curve a fallback without saying when it engages. It engages when the gauge reports 0% but voltage is non-zero.
