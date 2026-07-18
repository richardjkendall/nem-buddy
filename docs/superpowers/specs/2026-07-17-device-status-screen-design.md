# NEM Buddy — Device Status Screen Design

**Date:** 2026-07-17
**Status:** Approved design (pre-implementation)
**Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16 (ESP-IDF v5.5 + LVGL v9)
**Parent design:** `docs/superpowers/specs/2026-07-10-nem-buddy-realtime-monitor-design.md`

## Problem / scope

The device is gaining a 3.7V lithium battery (MX1.25 header, managed by the onboard
AXP2101 PMIC). Once it runs untethered there is no way to see how much charge is left,
nor to answer "why has the data stopped?" without a USB cable and a serial capture.

This design delivers a **device status screen**: a full-screen LVGL overlay showing
battery, network, device identity and data health, toggled by the **IO18** physical
button. It is diagnostic, read-only, and has no settings or controls.

**Approved mockup:** `.superpowers/brainstorm/*/content/layout-480.html` (gitignored —
reproduce via the brainstorming visual companion if needed). Drawn at the true 480×480
using the `ui_theme.h` palette, with both the healthy and degraded states.

## Hardware facts

From Waveshare's documentation for this board, plus the user's inspection of the physical
unit. Each item below is marked where it is **assumed** rather than observed — the
distinction matters, and Risks restates the open ones:

- **Three top-edge buttons**, silkscreened `IO18`, `PWR`, `BOOT` (user-verified on the
  physical unit):
  - `IO18` (Key3) — plain GPIO18 user button, externally pulled up, shorted to GND when
    pressed. No boot or power side effects.
  - `PWR` (Key1) — wired to the **AXP2101 PWRON pin, not to the ESP32**. Presses are not
    readable as a GPIO level; they surface as a latched PMIC event over I2C. Long press
    powers the board off.
  - `BOOT` (Key2) — GPIO0. Readable at runtime, but holding it across a reset enters USB
    download mode.
- **AXP2101 PMIC** on the **shared I2C bus (SDA=GPIO15, SCL=GPIO14)**, which the BSP
  already initialises for the CST9217 touch controller. Provides battery charge/discharge
  management, a fuel gauge, and a battery voltage ADC. Waveshare's docs confirm the chip
  and the shared bus but **do not state its I2C address**; `0x34` is the AXP2101's
  standard address from the datasheet and is *unverified on this board* — see Risks.
- Other devices sharing that bus: CST9217 (touch), QMI8658 (6-axis IMU), PCF85063 (RTC),
  ES8311/ES7210 (audio codec / mic ADC).
- The vendored BSP declares `BSP_CAPS_BUTTONS 0` and exposes **no battery or PMIC API** —
  both the button and the battery need new code.
- **Panel is 480×480**, per `BSP_LCD_H_RES` / `BSP_LCD_V_RES` in
  `components/esp32_s3_touch_amoled_2_16/include/bsp/display.h`, with a large corner
  radius. ⚠️ **The vendored BSP's `README.md` claims 410×502 — that is wrong**; it is the
  ESP32-S3-Touch-AMOLED-**2.06**'s panel, evidently copy-pasted. Trust `display.h`, not the
  README. (This error was caught only because the user knew the real figure.)

## Decisions

1. **Button = IO18 (GPIO18), not the middle button.** The user's initial request was for
   the middle button; on inspection the middle button is `PWR`, which the AXP2101 owns.
   Driving the screen from `PWR` would mean enabling and polling a PMIC interrupt-status
   register over I2C, coupling a UI toggle to power management, and living with
   long-press-powers-off. `IO18` is a plain debounced GPIO with no side effects. Decision:
   **use IO18 and accept the side-button ergonomics.** `PWR` retains its native
   power-on/off role; `BOOT` is left alone.
2. **AXP2101 driver = minimal, in-repo.** A ~100-line driver reading four things: chip ID,
   charge status, battery percentage, battery voltage. Rejected `XPowersLib` — it is C++
   and Arduino-flavoured, and pulling in a full power-management library to read a
   percentage is a poor trade for this codebase, which vendors deliberately and keeps its
   dependency surface at zero.
3. **Share the BSP's I2C bus.** The driver calls `bsp_i2c_get_handle()` and adds a device
   at the AXP2101's address (expected `0x34`, to be confirmed by scan — see Risks). It
   must **not** create a second master bus on GPIO14/15; the BSP already owns that bus.
4. **All I2C on the LVGL thread.** The overlay refreshes from a 2-second `lv_timer` that
   runs only while the screen is open. That timer executes in the LVGL task, which is also
   where touch I2C reads happen — so every transaction on the shared bus occurs on one
   thread and there is no bus contention to reason about. The button task never touches
   I2C; it only takes `bsp_display_lock()` and toggles. Reads cost 1–2ms.
5. **Toggle, stays open.** Press to show, press again to return. No auto-close timeout.
6. **Status is a pure overlay.** If the drill-in is open, status layers above it and
   toggling returns to the drill, not the dashboard. It does not disturb what is beneath.
7. **Show voltage alongside percentage.** The AXP2101 fuel gauge depends on its internal
   battery model and can read oddly on a cell it has not characterised. Showing voltage
   next to the percentage makes a nonsense percentage self-evident rather than silently
   wrong. A voltage→percentage curve in `core/` is the fallback if the gauge proves
   unreliable.
8. **Layout = 2×2 grid of boxed sections** (approved from mockup, see below). Header strip
   across the top, then Battery (top-left), Network (top-right), Data health
   (bottom-left), Device identity (bottom-right). The square panel gives each section a
   real box rather than a cramped strip, while battery still reads first from type size
   alone. Rejected a battery-hero layout with identity as a footer: it was the better
   choice on a *tall* panel, but that preference was an artefact of the wrong 410×502
   figure and does not survive the correction to 480×480.
9. **Truncate MAC and proxy URL.** Full values are not legible in a quarter of a 480×480
   panel. Show the MAC's last three octets (enough to identify a unit among a few) and the
   proxy host without its `/nem` path. The full values remain available over serial.

## Components

Following the existing split — pure logic in `core/` under unit test, I/O only in
`firmware/main/`:

| Unit | Responsibility | Depends on |
|---|---|---|
| `core/src/nem/battery.c` | Pure: `nem_batt_pct_from_mv()` — Li-ion voltage→% curve, clamped at both rails | nothing |
| `core/src/nem/timefmt.c` | Pure: `nem_fmt_ago(secs, buf, cap)` → "12s ago" / "3m ago" / "never" | nothing |
| `firmware/main/axp2101.{c,h}` | `axp2101_init()` (probe chip ID, enable battery ADC + fuel gauge); `axp2101_read(axp2101_state_t*)` → present / charging / percent / millivolts | BSP I2C handle |
| `firmware/main/buttons.{c,h}` | `buttons_start(cb)` — task polling GPIO18 every 20ms, debounced, fires `cb` on falling edge | driver/gpio |
| `firmware/main/ui_status.{c,h}` | `ui_status_toggle()`, `ui_status_is_open()` — overlay created on demand, destroyed on close; owns the 2s `lv_timer` | LVGL, the three above |

Mirrors the established `ui_drill` overlay pattern.

**Integration.** `main.c` calls `axp2101_init()` after `bsp_display_start()` (the BSP must
have brought the I2C bus up first) and `buttons_start(ui_status_toggle_cb)` alongside
`net_manager_start()`. The callback takes `bsp_display_lock(-1)`, calls
`ui_status_toggle()`, then unlocks — the button task must never touch LVGL unlocked.

## Data sources

| Section | Fields | Source |
|---|---|---|
| Battery | percent, voltage, charging state | `axp2101_read()` |
| Network | SSID, RSSI, IP address | new `wifi_ctrl_sta_info()` wrapping `esp_wifi_sta_get_ap_info()` + `esp_netif_get_ip_info()` |
| Identity | device ID, proxy URL, firmware version, Wi-Fi MAC | `net_creds_load()`; `esp_app_get_description()`; `esp_read_mac()` |
| Data health | time since last successful fetch, consecutive error count, uptime | new `data_task_health()` — requires `data_task.c` to start tracking these |

## Error handling

The status screen is what you look at when something is *already* wrong, so every section
degrades rather than crashes:

- AXP2101 absent or NAKing → "Battery: n/a", one logged warning. Never a boot failure.
- No cell connected (USB only) → "No battery".
- Wi-Fi down → "disconnected"; identity and health still render.
- Before the first successful fetch → "never".

## Board-specific constraints

Two hard-won constraints from this board's history, to be honoured from the start:

- **Safe-area inset.** The 480×480 AMOLED has a large corner radius and clips ~20px more on
  the **right** than the left. Content sits inside an inset of **20 left / 40 right / 20
  top / 20 bottom**, consistent with the dashboard's `pad_all 20` (440-wide content) and
  the drill-in's deliberately narrower `PHW 372` history plot. The large radius eats
  precisely where the 2×2 grid's outer corners sit, so the grid is inset inside a rounded
  safe area and the boxes are corner-rounded to clear it. **The exact radius is a mockup
  estimate (~96px) and must be checked on real glass** — it is the most likely detail to
  need nudging, and is cheap to adjust once running.
- **No `lv_obj_set_flex_grow()` for the battery bar.** Flex-grow does not size children
  proportionally on this board (a 0-value child rendered wide, producing a wrongly-sized
  dashboard fuel-mix bar). Set an explicit `LV_PCT` width computed from the percentage.

Also: LVGL UI must be built between `bsp_display_lock(-1)` and `bsp_display_unlock()`, and
`lv_color_hex()` is not a constant initializer — store colours as hex `uint32_t` in any
file-scope table.

## Testing

- **Unit (`core/`, Unity, as per `test_fuel`/`test_history`):** the voltage→% curve
  (including clamping at both rails) and the `nem_fmt_ago` formatter (including "never"
  and boundary rollovers).
- **On-device:** scan the shared I2C bus and confirm the AXP2101 chip ID reads back;
  confirm an IO18 press toggles the overlay; confirm the overlay renders inside the safe
  area.
- **Human UAT (only the user can do this):** with the battery installed, confirm the
  percentage tracks reality and moves when charging vs discharging.

## Findings (implemented & verified — supersedes the pre-build risks)

1. **PMIC confirmed on real hardware.** An I2C bus scan found the AXP2101 at `0x34`
   answering chip ID `0x4A` (both previously only assumed). The shared bus also carries
   `0x18` ES8311 codec, `0x40` ES7210 mic ADC, `0x51` PCF85063 RTC, `0x5A` CST9217 touch,
   `0x6B` QMI8658 IMU. The scanner is retained and called from `axp2101_init()` on an
   identify failure, so a bus dump prints exactly when one would be wanted.
2. **Register map confirmed:** `0x00` status1 (bit 3 present, bit 5 VBUS-good), `0x01`
   status2 (bits[6:5] charge direction), `0x30` bit 0 ADC enable, `0x34`/`0x35` 14-bit
   voltage in direct mV (auto-incrementing read, hardware-proven), `0xA4` percent. The
   driver fails **closed**: any read failure, `percent>100`, or a present cell's voltage
   outside 2500–4500mV returns `ESP_FAIL`, so the UI shows "n/a" rather than a plausible
   wrong value.
3. **Battery has three states, not two.** A full cell on USB reads `charging=0` (standby)
   yet is not on battery; the UI reads the VBUS bit and shows **Charging / USB power / On
   battery**. Verified on glass showing "USB power" when plugged in and full.
4. **Panel resolution & layout:** confirmed 480×480. The safe-area right inset was
   rebalanced on glass from 40px to 26px (left stays 20) — the original inset left the grid
   visibly left-of-centre. No corner clipping at the final geometry.
5. **Fuel-gauge fallback** engages when the gauge reports 0% against a healthy voltage,
   using the `core/` curve; voltage is shown alongside percent so a bad gauge reading is
   self-evident.

### Open residual

- The **actively-Charging** state (status2 bits[6:5]==`01`) has not been observed on
  hardware: it needs a partially-discharged cell on USB, and unplugging USB kills the
  serial link (power and console share the USB-C port). The VBUS-bit assignment is
  evidence-backed (only environmental bit set beyond battery-present while plugged) and
  matches XPowersLib; the on-glass "USB power" ⇄ "On battery" flip when unplugging is its
  confirmation path.

## Out of scope

- Any settings or controls on this screen — it is read-only diagnostics.
- A persistent battery indicator on the dashboard.
- Low-battery alerts, or any power-saving/sleep behaviour.
- Using `PWR` or `BOOT` for anything.
