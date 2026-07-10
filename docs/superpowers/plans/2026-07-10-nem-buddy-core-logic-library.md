# NEM Buddy — Plan 1: Core Logic Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a pure-C, host-testable library (`core/`) that parses the live AEMO and OpenElectricity feeds into normalized structs, accumulates today's price/demand history, and evaluates alert conditions — with no hardware or network dependency.

**Architecture:** A standalone CMake project under `core/` compiled with the host toolchain. Each concern is one focused module (`regions`, `config`, `timeutil`, `aemo_client`, `history`, `oe_client`, `alerts`) with a header in `core/include/nem/` and source in `core/src/`. JSON is parsed with cJSON (vendored, identical to the version ESP-IDF bundles). Tests use Unity (vendored) and run under CTest. Everything is fixed-size — no dynamic allocation beyond what cJSON does internally — so the modules drop straight into an ESP-IDF component in later plans.

**Tech Stack:** C11, CMake ≥ 3.16, CTest, cJSON (vendored single-file), Unity (vendored single-file).

## Global Constraints

- Language: **C11**, compiled with `-Wall -Wextra -Werror`.
- JSON library: **cJSON** only (matches ESP-IDF's bundled component). Vendored into `core/third_party/`.
- Test framework: **Unity** only. Vendored into `core/third_party/`.
- **No heap allocation in our modules** — all outputs are caller-provided fixed-size structs. cJSON's internal allocation is the only exception, and every `cJSON_Parse` is paired with `cJSON_Delete`.
- NEM regions are exactly five: `NSW1, QLD1, SA1, TAS1, VIC1`. Default home region: **VIC**.
- Alert threshold defaults (verbatim): spike `> $300/MWh`, extreme spike `> $1000/MWh`, negative `< $0/MWh`, high renewable `> 0.80` fraction. Per-region high-demand marks default to `0` (disabled) until configured.
- All public symbols are prefixed `nem_`. Public types end `_t`.
- Every module change ends with a commit.

---

## File Structure

```
core/
  CMakeLists.txt                 # host build + CTest wiring
  include/nem/
    regions.h                    # region enum + id/name lookups
    snapshot.h                   # normalized live-data structs
    config.h                     # config + thresholds + quiet hours
    timeutil.h                   # ISO8601 -> epoch, minute-of-day
    aemo_client.h                # parse ELEC_NEM_SUMMARY
    history.h                    # per-region intraday ring buffers
    fuel.h                       # fuel enum + fueltech mapping
    oe_client.h                  # parse OpenElectricity network data
    alerts.h                     # alert evaluation engine
  src/
    regions.c  config.c  timeutil.c  aemo_client.c
    history.c  fuel.c  oe_client.c  alerts.c
  third_party/
    cJSON.c  cJSON.h             # vendored
    unity.c  unity.h  unity_internals.h   # vendored
  test/
    fixtures/aemo_summary.json
    fixtures/oe_power_fueltech.json
    test_regions.c  test_config.c  test_timeutil.c  test_aemo_client.c
    test_history.c  test_fuel.c  test_oe_client.c  test_alerts.c
```

Each task adds one module + its test and wires it into CTest.

---

## Task 1: Project scaffolding + `regions` module

Folds in all one-time setup (CMake, vendored cJSON/Unity, CTest) because the `regions` test is the first thing that needs them.

**Files:**
- Create: `core/CMakeLists.txt`
- Create: `core/third_party/cJSON.c`, `core/third_party/cJSON.h` (vendored)
- Create: `core/third_party/unity.c`, `core/third_party/unity.h`, `core/third_party/unity_internals.h` (vendored)
- Create: `core/include/nem/regions.h`
- Create: `core/src/regions.c`
- Test: `core/test/test_regions.c`

**Interfaces:**
- Produces:
  - `typedef enum { NEM_REGION_NSW=0, NEM_REGION_QLD, NEM_REGION_SA, NEM_REGION_TAS, NEM_REGION_VIC, NEM_REGION_COUNT } nem_region_t;`
  - `const char *nem_region_id(nem_region_t r);` → `"NSW1"`… (`"?"` if out of range)
  - `const char *nem_region_name(nem_region_t r);` → `"NSW"`… (`"?"` if out of range)
  - `nem_region_t nem_region_from_id(const char *region_id);` → maps `"NSW1"`→`NEM_REGION_NSW`; returns `NEM_REGION_COUNT` if unknown/NULL
  - `nem_region_t nem_region_from_short(const char *short_name);` → maps `"NSW"`→`NEM_REGION_NSW`; returns `NEM_REGION_COUNT` if unknown/NULL

- [ ] **Step 1: Vendor cJSON and Unity**

Run:
```bash
mkdir -p core/third_party core/include/nem core/src core/test/fixtures
curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c -o core/third_party/cJSON.c
curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h -o core/third_party/cJSON.h
curl -fsSL https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v2.6.0/src/unity.c -o core/third_party/unity.c
curl -fsSL https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v2.6.0/src/unity.h -o core/third_party/unity.h
curl -fsSL https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v2.6.0/src/unity_internals.h -o core/third_party/unity_internals.h
```
Expected: five files downloaded, non-zero size (`ls -l core/third_party`).

- [ ] **Step 2: Write `core/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(nem_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# Vendored deps (kept lint-clean by isolating them from our -Werror where needed)
add_library(cjson STATIC third_party/cJSON.c)
target_include_directories(cjson PUBLIC third_party)
target_compile_options(cjson PRIVATE -w)   # don't fail our build on vendored warnings

add_library(unity STATIC third_party/unity.c)
target_include_directories(unity PUBLIC third_party)
target_compile_options(unity PRIVATE -w)

# Our library — sources are added by each task as modules land.
add_library(nem_core STATIC
  src/regions.c
)
target_include_directories(nem_core PUBLIC include)
target_link_libraries(nem_core PUBLIC cjson)

enable_testing()

# Helper: register a Unity test executable.
function(nem_add_test name)
  add_executable(${name} test/${name}.c)
  target_link_libraries(${name} PRIVATE nem_core unity)
  target_include_directories(${name} PRIVATE include third_party)
  # Fixtures are found relative to this dir at runtime.
  target_compile_definitions(${name} PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
  add_test(NAME ${name} COMMAND ${name})
endfunction()

nem_add_test(test_regions)
```

- [ ] **Step 3: Write the failing test `core/test/test_regions.c`**

```c
#include "unity.h"
#include "nem/regions.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_id_and_name_roundtrip(void) {
    TEST_ASSERT_EQUAL_STRING("VIC1", nem_region_id(NEM_REGION_VIC));
    TEST_ASSERT_EQUAL_STRING("VIC",  nem_region_name(NEM_REGION_VIC));
    TEST_ASSERT_EQUAL_STRING("SA1",  nem_region_id(NEM_REGION_SA));
}

static void test_from_id(void) {
    TEST_ASSERT_EQUAL_INT(NEM_REGION_NSW, nem_region_from_id("NSW1"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_TAS, nem_region_from_id("TAS1"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_COUNT, nem_region_from_id("WA1"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_COUNT, nem_region_from_id(NULL));
}

static void test_from_short(void) {
    TEST_ASSERT_EQUAL_INT(NEM_REGION_QLD, nem_region_from_short("QLD"));
    TEST_ASSERT_EQUAL_INT(NEM_REGION_COUNT, nem_region_from_short("ACT"));
}

static void test_out_of_range(void) {
    TEST_ASSERT_EQUAL_STRING("?", nem_region_id(NEM_REGION_COUNT));
    TEST_ASSERT_EQUAL_STRING("?", nem_region_name(NEM_REGION_COUNT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_id_and_name_roundtrip);
    RUN_TEST(test_from_id);
    RUN_TEST(test_from_short);
    RUN_TEST(test_out_of_range);
    return UNITY_END();
}
```

- [ ] **Step 4: Write the header `core/include/nem/regions.h`**

```c
#ifndef NEM_REGIONS_H
#define NEM_REGIONS_H

typedef enum {
    NEM_REGION_NSW = 0,
    NEM_REGION_QLD,
    NEM_REGION_SA,
    NEM_REGION_TAS,
    NEM_REGION_VIC,
    NEM_REGION_COUNT
} nem_region_t;

const char  *nem_region_id(nem_region_t r);
const char  *nem_region_name(nem_region_t r);
nem_region_t nem_region_from_id(const char *region_id);
nem_region_t nem_region_from_short(const char *short_name);

#endif
```

- [ ] **Step 5: Run the test to verify it fails**

Run:
```bash
cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build --output-on-failure
```
Expected: **link/compile failure** — `nem_region_id` and friends undefined (implementation not written yet).

- [ ] **Step 6: Write `core/src/regions.c`**

```c
#include "nem/regions.h"
#include <string.h>

static const char *const IDS[NEM_REGION_COUNT]    = { "NSW1", "QLD1", "SA1", "TAS1", "VIC1" };
static const char *const NAMES[NEM_REGION_COUNT]   = { "NSW",  "QLD",  "SA",  "TAS",  "VIC"  };

const char *nem_region_id(nem_region_t r) {
    return (r >= 0 && r < NEM_REGION_COUNT) ? IDS[r] : "?";
}

const char *nem_region_name(nem_region_t r) {
    return (r >= 0 && r < NEM_REGION_COUNT) ? NAMES[r] : "?";
}

nem_region_t nem_region_from_id(const char *region_id) {
    if (!region_id) return NEM_REGION_COUNT;
    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        if (strcmp(region_id, IDS[i]) == 0) return (nem_region_t)i;
    }
    return NEM_REGION_COUNT;
}

nem_region_t nem_region_from_short(const char *short_name) {
    if (!short_name) return NEM_REGION_COUNT;
    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        if (strcmp(short_name, NAMES[i]) == 0) return (nem_region_t)i;
    }
    return NEM_REGION_COUNT;
}
```

- [ ] **Step 7: Run the test to verify it passes**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build --output-on-failure
```
Expected: `test_regions` **passes**, `100% tests passed`.

- [ ] **Step 8: Commit**

```bash
git add core/ .gitignore
git commit -m "feat(core): scaffold host test build + regions module"
```
Add to `.gitignore` first if not present: `core/build/`.

---

## Task 2: `snapshot.h` + `config` module

**Files:**
- Create: `core/include/nem/snapshot.h` (types only, no `.c`)
- Create: `core/include/nem/config.h`
- Create: `core/src/config.c`
- Modify: `core/CMakeLists.txt` (add `src/config.c` to `nem_core`; add `nem_add_test(test_config)`)
- Test: `core/test/test_config.c`

**Interfaces:**
- Consumes: `nem_region_t` (Task 1).
- Produces (`snapshot.h`):
  - `NEM_MAX_INTERCONNECTORS` = 6
  - `typedef struct { char name[24]; double value; } nem_interconnector_flow_t;`
  - `nem_region_snapshot_t { nem_region_t region; bool valid; double price; double demand_mw; double net_interchange; long long settlement_epoch; int interconnector_count; nem_interconnector_flow_t interconnectors[NEM_MAX_INTERCONNECTORS]; }`
  - `nem_snapshot_t { nem_region_snapshot_t regions[NEM_REGION_COUNT]; }`
- Produces (`config.h`):
  - `nem_thresholds_t { double spike_price; double extreme_spike_price; double negative_price; double high_demand_mw[NEM_REGION_COUNT]; double high_renewable_frac; }`
  - `nem_config_t { nem_region_t home_region; nem_thresholds_t thresholds; unsigned char quiet_start_hour; unsigned char quiet_end_hour; bool chime_muted; unsigned char brightness; unsigned short alert_auto_dismiss_s; }`
  - `void nem_config_defaults(nem_config_t *cfg);`
  - `bool nem_config_validate(const nem_config_t *cfg);`
  - `bool nem_config_in_quiet_hours(const nem_config_t *cfg, int hour);`
  - `bool nem_config_should_chime(const nem_config_t *cfg, int hour);` → `!chime_muted && !in_quiet_hours(hour)`

- [ ] **Step 1: Write `core/include/nem/snapshot.h`**

```c
#ifndef NEM_SNAPSHOT_H
#define NEM_SNAPSHOT_H

#include <stdbool.h>
#include "nem/regions.h"

#define NEM_MAX_INTERCONNECTORS 6

typedef struct {
    char   name[24];
    double value;   /* MW; sign indicates direction */
} nem_interconnector_flow_t;

typedef struct {
    nem_region_t region;
    bool         valid;
    double       price;            /* $/MWh (RRP)     */
    double       demand_mw;        /* TOTALDEMAND     */
    double       net_interchange;  /* NETINTERCHANGE  */
    long long    settlement_epoch; /* unix seconds    */
    int          interconnector_count;
    nem_interconnector_flow_t interconnectors[NEM_MAX_INTERCONNECTORS];
} nem_region_snapshot_t;

typedef struct {
    nem_region_snapshot_t regions[NEM_REGION_COUNT];
} nem_snapshot_t;

#endif
```

- [ ] **Step 2: Write the failing test `core/test/test_config.c`**

```c
#include "unity.h"
#include "nem/config.h"

void setUp(void) {}
void tearDown(void) {}

static void test_defaults(void) {
    nem_config_t c;
    nem_config_defaults(&c);
    TEST_ASSERT_EQUAL_INT(NEM_REGION_VIC, c.home_region);
    TEST_ASSERT_EQUAL_DOUBLE(300.0,  c.thresholds.spike_price);
    TEST_ASSERT_EQUAL_DOUBLE(1000.0, c.thresholds.extreme_spike_price);
    TEST_ASSERT_EQUAL_DOUBLE(0.0,    c.thresholds.negative_price);
    TEST_ASSERT_EQUAL_DOUBLE(0.80,   c.thresholds.high_renewable_frac);
    TEST_ASSERT_EQUAL_UINT8(30, c.alert_auto_dismiss_s == 30 ? 30 : 0);
    TEST_ASSERT_TRUE(nem_config_validate(&c));
}

static void test_validate_rejects_bad(void) {
    nem_config_t c; nem_config_defaults(&c);
    c.brightness = 200;                 /* > 100 */
    TEST_ASSERT_FALSE(nem_config_validate(&c));
    nem_config_defaults(&c);
    c.thresholds.extreme_spike_price = 100.0; /* below spike */
    TEST_ASSERT_FALSE(nem_config_validate(&c));
}

static void test_quiet_hours_wrap(void) {
    nem_config_t c; nem_config_defaults(&c); /* quiet 22 -> 7 */
    TEST_ASSERT_TRUE(nem_config_in_quiet_hours(&c, 23));
    TEST_ASSERT_TRUE(nem_config_in_quiet_hours(&c, 3));
    TEST_ASSERT_TRUE(nem_config_in_quiet_hours(&c, 22));  /* inclusive start */
    TEST_ASSERT_FALSE(nem_config_in_quiet_hours(&c, 7));  /* exclusive end   */
    TEST_ASSERT_FALSE(nem_config_in_quiet_hours(&c, 12));
}

static void test_should_chime(void) {
    nem_config_t c; nem_config_defaults(&c);
    TEST_ASSERT_TRUE(nem_config_should_chime(&c, 12));
    TEST_ASSERT_FALSE(nem_config_should_chime(&c, 2));  /* quiet hours */
    c.chime_muted = true;
    TEST_ASSERT_FALSE(nem_config_should_chime(&c, 12)); /* muted        */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_validate_rejects_bad);
    RUN_TEST(test_quiet_hours_wrap);
    RUN_TEST(test_should_chime);
    return UNITY_END();
}
```

- [ ] **Step 3: Write `core/include/nem/config.h`**

```c
#ifndef NEM_CONFIG_H
#define NEM_CONFIG_H

#include <stdbool.h>
#include "nem/regions.h"

typedef struct {
    double spike_price;
    double extreme_spike_price;
    double negative_price;
    double high_demand_mw[NEM_REGION_COUNT];
    double high_renewable_frac;
} nem_thresholds_t;

typedef struct {
    nem_region_t   home_region;
    nem_thresholds_t thresholds;
    unsigned char  quiet_start_hour;   /* 0..23 */
    unsigned char  quiet_end_hour;     /* 0..23 */
    bool           chime_muted;
    unsigned char  brightness;         /* 0..100 */
    unsigned short alert_auto_dismiss_s;
} nem_config_t;

void nem_config_defaults(nem_config_t *cfg);
bool nem_config_validate(const nem_config_t *cfg);
bool nem_config_in_quiet_hours(const nem_config_t *cfg, int hour);
bool nem_config_should_chime(const nem_config_t *cfg, int hour);

#endif
```

- [ ] **Step 4: Wire into CMake and run to verify failure**

Edit `core/CMakeLists.txt`: add `src/config.c` to the `nem_core` source list and add `nem_add_test(test_config)` after the existing `nem_add_test(test_regions)` line.

Run:
```bash
cmake -S core -B core/build && cmake --build core/build
```
Expected: **failure** — `core/src/config.c` does not exist yet / functions undefined.

- [ ] **Step 5: Write `core/src/config.c`**

```c
#include "nem/config.h"

void nem_config_defaults(nem_config_t *cfg) {
    cfg->home_region = NEM_REGION_VIC;
    cfg->thresholds.spike_price = 300.0;
    cfg->thresholds.extreme_spike_price = 1000.0;
    cfg->thresholds.negative_price = 0.0;
    cfg->thresholds.high_renewable_frac = 0.80;
    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        cfg->thresholds.high_demand_mw[i] = 0.0; /* disabled until set */
    }
    cfg->quiet_start_hour = 22;
    cfg->quiet_end_hour = 7;
    cfg->chime_muted = false;
    cfg->brightness = 80;
    cfg->alert_auto_dismiss_s = 30;
}

bool nem_config_validate(const nem_config_t *cfg) {
    if (cfg->home_region < 0 || cfg->home_region >= NEM_REGION_COUNT) return false;
    if (cfg->brightness > 100) return false;
    if (cfg->quiet_start_hour > 23 || cfg->quiet_end_hour > 23) return false;
    if (cfg->thresholds.extreme_spike_price < cfg->thresholds.spike_price) return false;
    if (cfg->thresholds.high_renewable_frac < 0.0 || cfg->thresholds.high_renewable_frac > 1.0) return false;
    if (cfg->alert_auto_dismiss_s == 0) return false;
    return true;
}

bool nem_config_in_quiet_hours(const nem_config_t *cfg, int hour) {
    int s = cfg->quiet_start_hour, e = cfg->quiet_end_hour;
    if (s == e) return false;             /* no quiet window */
    if (s < e) return hour >= s && hour < e;
    return hour >= s || hour < e;          /* wraps midnight  */
}

bool nem_config_should_chime(const nem_config_t *cfg, int hour) {
    return !cfg->chime_muted && !nem_config_in_quiet_hours(cfg, hour);
}
```

- [ ] **Step 6: Build and run to verify pass**

Run:
```bash
cmake --build core/build && ctest --test-dir core/build --output-on-failure
```
Expected: both `test_regions` and `test_config` pass.

- [ ] **Step 7: Commit**

```bash
git add core/
git commit -m "feat(core): add snapshot types and config module"
```

---

## Task 3: `timeutil` — ISO8601 → epoch

AEMO's `SETTLEMENTDATE` looks like `"2026-07-10T19:25:00"` (no timezone suffix). We treat it as UTC-naive: all samples share the same clock, so bucketing and diffing are correct regardless of the absolute offset.

**Files:**
- Create: `core/include/nem/timeutil.h`
- Create: `core/src/timeutil.c`
- Modify: `core/CMakeLists.txt` (add `src/timeutil.c`; `nem_add_test(test_timeutil)`)
- Test: `core/test/test_timeutil.c`

**Interfaces:**
- Produces:
  - `bool nem_parse_iso8601(const char *s, long long *epoch_out);` → parses `YYYY-MM-DDTHH:MM:SS`; returns false on malformed input.
  - `int nem_minute_of_day(long long epoch);` → `0..1439`
  - `int nem_day_index(long long epoch);` → integer days since epoch (for day-rollover detection)

- [ ] **Step 1: Write the failing test `core/test/test_timeutil.c`**

```c
#include "unity.h"
#include "nem/timeutil.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse_known(void) {
    long long e = 0;
    /* 2026-07-10T19:25:00 UTC = 1783711500 */
    TEST_ASSERT_TRUE(nem_parse_iso8601("2026-07-10T19:25:00", &e));
    TEST_ASSERT_EQUAL_INT64(1783711500LL, e);
}

static void test_parse_epoch_zero(void) {
    long long e = -1;
    TEST_ASSERT_TRUE(nem_parse_iso8601("1970-01-01T00:00:00", &e));
    TEST_ASSERT_EQUAL_INT64(0LL, e);
}

static void test_parse_rejects_malformed(void) {
    long long e = 0;
    TEST_ASSERT_FALSE(nem_parse_iso8601("2026-07-10 19:25:00", &e)); /* space, not T */
    TEST_ASSERT_FALSE(nem_parse_iso8601("not-a-date", &e));
    TEST_ASSERT_FALSE(nem_parse_iso8601(NULL, &e));
}

static void test_minute_and_day(void) {
    long long e;
    nem_parse_iso8601("2026-07-10T19:25:00", &e);
    TEST_ASSERT_EQUAL_INT(19 * 60 + 25, nem_minute_of_day(e));
    long long a, b;
    nem_parse_iso8601("2026-07-10T23:59:00", &a);
    nem_parse_iso8601("2026-07-11T00:01:00", &b);
    TEST_ASSERT_EQUAL_INT(1, nem_day_index(b) - nem_day_index(a));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_known);
    RUN_TEST(test_parse_epoch_zero);
    RUN_TEST(test_parse_rejects_malformed);
    RUN_TEST(test_minute_and_day);
    return UNITY_END();
}
```

- [ ] **Step 2: Write `core/include/nem/timeutil.h`**

```c
#ifndef NEM_TIMEUTIL_H
#define NEM_TIMEUTIL_H

#include <stdbool.h>

bool nem_parse_iso8601(const char *s, long long *epoch_out);
int  nem_minute_of_day(long long epoch);
int  nem_day_index(long long epoch);

#endif
```

- [ ] **Step 3: Wire into CMake and run to verify failure**

Add `src/timeutil.c` to `nem_core` and `nem_add_test(test_timeutil)`.
Run: `cmake -S core -B core/build && cmake --build core/build`
Expected: failure — `src/timeutil.c` missing.

- [ ] **Step 4: Write `core/src/timeutil.c`**

```c
#include "nem/timeutil.h"
#include <stdio.h>

/* Days from 1970-01-01 to civil date y-m-d. Howard Hinnant's algorithm. */
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

bool nem_parse_iso8601(const char *s, long long *epoch_out) {
    if (!s) return false;
    int y, mo, d, h, mi, se;
    char t;
    /* Require the literal 'T' separator between date and time. */
    if (sscanf(s, "%4d-%2d-%2d%c%2d:%2d:%2d", &y, &mo, &d, &t, &h, &mi, &se) != 7)
        return false;
    if (t != 'T') return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (h > 23 || mi > 59 || se > 60) return false;
    long long days = days_from_civil(y, (unsigned)mo, (unsigned)d);
    *epoch_out = days * 86400LL + h * 3600LL + mi * 60LL + se;
    return true;
}

int nem_minute_of_day(long long epoch) {
    long long secs = epoch % 86400;
    if (secs < 0) secs += 86400;
    return (int)(secs / 60);
}

int nem_day_index(long long epoch) {
    long long d = epoch / 86400;
    if (epoch < 0 && epoch % 86400 != 0) d -= 1;
    return (int)d;
}
```

- [ ] **Step 5: Build and run to verify pass**

Run: `cmake --build core/build && ctest --test-dir core/build --output-on-failure`
Expected: `test_timeutil` passes (all suites green).

- [ ] **Step 6: Commit**

```bash
git add core/
git commit -m "feat(core): add ISO8601 time parsing utilities"
```

---

## Task 4: `aemo_client` — parse `ELEC_NEM_SUMMARY`

Real shape (confirmed from the live feed): root object with key `"ELEC_NEM_SUMMARY"` → array; each element has `REGIONID`, `PRICE`, `TOTALDEMAND`, `NETINTERCHANGE`, `SETTLEMENTDATE`, and `INTERCONNECTORFLOWS` — the last is a **string** containing a JSON array of `{"name":...,"value":...}` objects, so it must be parsed a second time.

**Files:**
- Create: `core/include/nem/aemo_client.h`
- Create: `core/src/aemo_client.c`
- Create: `core/test/fixtures/aemo_summary.json`
- Modify: `core/CMakeLists.txt` (add `src/aemo_client.c`; `nem_add_test(test_aemo_client)`)
- Test: `core/test/test_aemo_client.c`

**Interfaces:**
- Consumes: `nem_snapshot_t` (Task 2), `nem_region_from_id` (Task 1), `nem_parse_iso8601` (Task 3), cJSON.
- Produces:
  - `bool nem_aemo_parse_summary(const char *json, nem_snapshot_t *out);` → zeroes `out`, marks each parsed region `valid=true`; returns true if ≥1 region parsed.

- [ ] **Step 1: Write the fixture `core/test/fixtures/aemo_summary.json`**

```json
{
  "ELEC_NEM_SUMMARY": [
    {
      "SETTLEMENTDATE": "2026-07-10T19:25:00",
      "REGIONID": "VIC1",
      "PRICE": 96.76914,
      "PRICE_STATUS": "FIRM",
      "TOTALDEMAND": 6240.12,
      "NETINTERCHANGE": -420.5,
      "INTERCONNECTORFLOWS": "[{\"name\":\"VIC1-NSW1\",\"value\":-311.2},{\"name\":\"V-SA\",\"value\":-109.3}]"
    },
    {
      "SETTLEMENTDATE": "2026-07-10T19:25:00",
      "REGIONID": "SA1",
      "PRICE": -18.4,
      "PRICE_STATUS": "FIRM",
      "TOTALDEMAND": 1320.0,
      "NETINTERCHANGE": 88.1,
      "INTERCONNECTORFLOWS": "[{\"name\":\"V-SA\",\"value\":109.3}]"
    }
  ],
  "ELEC_NEM_SUMMARY_PRICES": [],
  "ELEC_NEM_SUMMARY_MARKET_NOTICE": []
}
```

- [ ] **Step 2: Write the failing test `core/test/test_aemo_client.c`**

```c
#include "unity.h"
#include "nem/aemo_client.h"
#include "nem/timeutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static char *read_fixture(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f); buf[n] = 0; fclose(f);
    return buf;
}

static void test_parses_two_regions(void) {
    char *json = read_fixture("aemo_summary.json");
    nem_snapshot_t s;
    TEST_ASSERT_TRUE(nem_aemo_parse_summary(json, &s));

    const nem_region_snapshot_t *vic = &s.regions[NEM_REGION_VIC];
    TEST_ASSERT_TRUE(vic->valid);
    TEST_ASSERT_EQUAL_DOUBLE(96.76914, vic->price);
    TEST_ASSERT_EQUAL_DOUBLE(6240.12, vic->demand_mw);
    TEST_ASSERT_EQUAL_DOUBLE(-420.5, vic->net_interchange);

    long long expect; nem_parse_iso8601("2026-07-10T19:25:00", &expect);
    TEST_ASSERT_EQUAL_INT64(expect, vic->settlement_epoch);

    TEST_ASSERT_EQUAL_INT(2, vic->interconnector_count);
    TEST_ASSERT_EQUAL_STRING("VIC1-NSW1", vic->interconnectors[0].name);
    TEST_ASSERT_EQUAL_DOUBLE(-311.2, vic->interconnectors[0].value);

    const nem_region_snapshot_t *sa = &s.regions[NEM_REGION_SA];
    TEST_ASSERT_TRUE(sa->valid);
    TEST_ASSERT_EQUAL_DOUBLE(-18.4, sa->price);
    TEST_ASSERT_EQUAL_INT(1, sa->interconnector_count);

    /* Regions absent from the feed stay invalid. */
    TEST_ASSERT_FALSE(s.regions[NEM_REGION_QLD].valid);
    free(json);
}

static void test_rejects_garbage(void) {
    nem_snapshot_t s;
    TEST_ASSERT_FALSE(nem_aemo_parse_summary("{not json", &s));
    TEST_ASSERT_FALSE(nem_aemo_parse_summary("{\"ELEC_NEM_SUMMARY\":[]}", &s));
    TEST_ASSERT_FALSE(nem_aemo_parse_summary(NULL, &s));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_two_regions);
    RUN_TEST(test_rejects_garbage);
    return UNITY_END();
}
```

- [ ] **Step 3: Write `core/include/nem/aemo_client.h`**

```c
#ifndef NEM_AEMO_CLIENT_H
#define NEM_AEMO_CLIENT_H

#include <stdbool.h>
#include "nem/snapshot.h"

bool nem_aemo_parse_summary(const char *json, nem_snapshot_t *out);

#endif
```

- [ ] **Step 4: Wire into CMake and run to verify failure**

Add `src/aemo_client.c` to `nem_core` and `nem_add_test(test_aemo_client)`.
Run: `cmake -S core -B core/build && cmake --build core/build`
Expected: failure — `src/aemo_client.c` missing.

- [ ] **Step 5: Write `core/src/aemo_client.c`**

```c
#include "nem/aemo_client.h"
#include "nem/regions.h"
#include "nem/timeutil.h"
#include "cJSON.h"
#include <string.h>

static double num(const cJSON *obj, const char *key) {
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) ? n->valuedouble : 0.0;
}

static void parse_flows(const char *flows_json, nem_region_snapshot_t *r) {
    r->interconnector_count = 0;
    if (!flows_json) return;
    cJSON *arr = cJSON_Parse(flows_json);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        if (r->interconnector_count >= NEM_MAX_INTERCONNECTORS) break;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(el, "name");
        nem_interconnector_flow_t *f = &r->interconnectors[r->interconnector_count];
        if (cJSON_IsString(name) && name->valuestring) {
            strncpy(f->name, name->valuestring, sizeof(f->name) - 1);
            f->name[sizeof(f->name) - 1] = 0;
        } else {
            f->name[0] = 0;
        }
        f->value = num(el, "value");
        r->interconnector_count++;
    }
    cJSON_Delete(arr);
}

bool nem_aemo_parse_summary(const char *json, nem_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < NEM_REGION_COUNT; i++) out->regions[i].region = (nem_region_t)i;
    if (!json) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "ELEC_NEM_SUMMARY");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return false; }

    int parsed = 0;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(el, "REGIONID");
        if (!cJSON_IsString(rid)) continue;
        nem_region_t reg = nem_region_from_id(rid->valuestring);
        if (reg >= NEM_REGION_COUNT) continue;

        nem_region_snapshot_t *r = &out->regions[reg];
        r->valid = true;
        r->price = num(el, "PRICE");
        r->demand_mw = num(el, "TOTALDEMAND");
        r->net_interchange = num(el, "NETINTERCHANGE");

        const cJSON *sd = cJSON_GetObjectItemCaseSensitive(el, "SETTLEMENTDATE");
        r->settlement_epoch = 0;
        if (cJSON_IsString(sd)) nem_parse_iso8601(sd->valuestring, &r->settlement_epoch);

        const cJSON *fl = cJSON_GetObjectItemCaseSensitive(el, "INTERCONNECTORFLOWS");
        parse_flows(cJSON_IsString(fl) ? fl->valuestring : NULL, r);
        parsed++;
    }
    cJSON_Delete(root);
    return parsed > 0;
}
```

- [ ] **Step 6: Build and run to verify pass**

Run: `cmake --build core/build && ctest --test-dir core/build --output-on-failure`
Expected: `test_aemo_client` passes.

- [ ] **Step 7: Commit**

```bash
git add core/
git commit -m "feat(core): parse AEMO ELEC_NEM_SUMMARY into snapshots"
```

---

## Task 5: `history` — intraday ring buffers

Because we poll AEMO every 60 s, the device builds today's price/demand curves itself — no history API needed. Samples bucket into 288 five-minute slots; a new day resets the region's buffer.

**Files:**
- Create: `core/include/nem/history.h`
- Create: `core/src/history.c`
- Modify: `core/CMakeLists.txt` (add `src/history.c`; `nem_add_test(test_history)`)
- Test: `core/test/test_history.c`

**Interfaces:**
- Consumes: `nem_snapshot_t` (Task 2), `nem_minute_of_day`/`nem_day_index` (Task 3).
- Produces:
  - `#define NEM_HISTORY_SLOTS 288`
  - `nem_region_history_t { double price[NEM_HISTORY_SLOTS]; double demand[NEM_HISTORY_SLOTS]; bool filled[NEM_HISTORY_SLOTS]; int day_index; }`
  - `nem_history_t { nem_region_history_t regions[NEM_REGION_COUNT]; }`
  - `void nem_history_init(nem_history_t *h);`
  - `void nem_history_add(nem_history_t *h, const nem_snapshot_t *snap, long long epoch);`
  - `int  nem_history_filled_count(const nem_region_history_t *rh);`

- [ ] **Step 1: Write the failing test `core/test/test_history.c`**

```c
#include "unity.h"
#include "nem/history.h"
#include "nem/timeutil.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static nem_snapshot_t make_snap(double vic_price, double vic_demand) {
    nem_snapshot_t s; memset(&s, 0, sizeof(s));
    for (int i = 0; i < NEM_REGION_COUNT; i++) s.regions[i].region = (nem_region_t)i;
    s.regions[NEM_REGION_VIC].valid = true;
    s.regions[NEM_REGION_VIC].price = vic_price;
    s.regions[NEM_REGION_VIC].demand_mw = vic_demand;
    return s;
}

static void test_add_and_bucket(void) {
    nem_history_t h; nem_history_init(&h);
    long long e; nem_parse_iso8601("2026-07-10T19:25:00", &e); /* minute 1165 -> slot 233 */
    nem_snapshot_t s = make_snap(96.7, 6240.0);
    nem_history_add(&h, &s, e);

    const nem_region_history_t *vic = &h.regions[NEM_REGION_VIC];
    int slot = (19 * 60 + 25) / 5;
    TEST_ASSERT_TRUE(vic->filled[slot]);
    TEST_ASSERT_EQUAL_DOUBLE(96.7, vic->price[slot]);
    TEST_ASSERT_EQUAL_INT(1, nem_history_filled_count(vic));
}

static void test_same_slot_overwrites(void) {
    nem_history_t h; nem_history_init(&h);
    long long a, b;
    nem_parse_iso8601("2026-07-10T19:25:00", &a);
    nem_parse_iso8601("2026-07-10T19:27:00", &b); /* same 5-min slot */
    nem_history_add(&h, &(nem_snapshot_t){0}, a); /* invalid region -> skipped */
    nem_snapshot_t s1 = make_snap(50.0, 100.0);
    nem_snapshot_t s2 = make_snap(60.0, 110.0);
    nem_history_add(&h, &s1, a);
    nem_history_add(&h, &s2, b);
    int slot = (19 * 60 + 25) / 5;
    TEST_ASSERT_EQUAL_DOUBLE(60.0, h.regions[NEM_REGION_VIC].price[slot]);
    TEST_ASSERT_EQUAL_INT(1, nem_history_filled_count(&h.regions[NEM_REGION_VIC]));
}

static void test_new_day_resets(void) {
    nem_history_t h; nem_history_init(&h);
    long long d1, d2;
    nem_parse_iso8601("2026-07-10T23:55:00", &d1);
    nem_parse_iso8601("2026-07-11T00:05:00", &d2);
    nem_snapshot_t s = make_snap(80.0, 5000.0);
    nem_history_add(&h, &s, d1);
    nem_history_add(&h, &s, d2);
    /* After rollover only the new day's single sample remains. */
    TEST_ASSERT_EQUAL_INT(1, nem_history_filled_count(&h.regions[NEM_REGION_VIC]));
    int slot_late = (23 * 60 + 55) / 5;
    TEST_ASSERT_FALSE(h.regions[NEM_REGION_VIC].filled[slot_late]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_and_bucket);
    RUN_TEST(test_same_slot_overwrites);
    RUN_TEST(test_new_day_resets);
    return UNITY_END();
}
```

- [ ] **Step 2: Write `core/include/nem/history.h`**

```c
#ifndef NEM_HISTORY_H
#define NEM_HISTORY_H

#include <stdbool.h>
#include "nem/snapshot.h"

#define NEM_HISTORY_SLOTS 288  /* 5-minute slots across 24h */

typedef struct {
    double price[NEM_HISTORY_SLOTS];
    double demand[NEM_HISTORY_SLOTS];
    bool   filled[NEM_HISTORY_SLOTS];
    int    day_index;
} nem_region_history_t;

typedef struct {
    nem_region_history_t regions[NEM_REGION_COUNT];
} nem_history_t;

void nem_history_init(nem_history_t *h);
void nem_history_add(nem_history_t *h, const nem_snapshot_t *snap, long long epoch);
int  nem_history_filled_count(const nem_region_history_t *rh);

#endif
```

- [ ] **Step 3: Wire into CMake and run to verify failure**

Add `src/history.c` to `nem_core` and `nem_add_test(test_history)`.
Run: `cmake -S core -B core/build && cmake --build core/build`
Expected: failure — `src/history.c` missing.

- [ ] **Step 4: Write `core/src/history.c`**

```c
#include "nem/history.h"
#include "nem/timeutil.h"
#include <string.h>

void nem_history_init(nem_history_t *h) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < NEM_REGION_COUNT; i++) h->regions[i].day_index = -1;
}

static void reset_region(nem_region_history_t *rh, int day) {
    memset(rh->price, 0, sizeof(rh->price));
    memset(rh->demand, 0, sizeof(rh->demand));
    memset(rh->filled, 0, sizeof(rh->filled));
    rh->day_index = day;
}

void nem_history_add(nem_history_t *h, const nem_snapshot_t *snap, long long epoch) {
    int day = nem_day_index(epoch);
    int slot = nem_minute_of_day(epoch) / 5;
    if (slot < 0 || slot >= NEM_HISTORY_SLOTS) return;

    for (int i = 0; i < NEM_REGION_COUNT; i++) {
        const nem_region_snapshot_t *rs = &snap->regions[i];
        if (!rs->valid) continue;
        nem_region_history_t *rh = &h->regions[i];
        if (rh->day_index != day) reset_region(rh, day);
        rh->price[slot] = rs->price;
        rh->demand[slot] = rs->demand_mw;
        rh->filled[slot] = true;
    }
}

int nem_history_filled_count(const nem_region_history_t *rh) {
    int n = 0;
    for (int i = 0; i < NEM_HISTORY_SLOTS; i++) if (rh->filled[i]) n++;
    return n;
}
```

- [ ] **Step 5: Build and run to verify pass**

Run: `cmake --build core/build && ctest --test-dir core/build --output-on-failure`
Expected: `test_history` passes.

- [ ] **Step 6: Commit**

```bash
git add core/
git commit -m "feat(core): add intraday history ring buffers"
```

---

## Task 6: `fuel` + `oe_client` — OpenElectricity generation mix

Real shape (confirmed from the OpenElectricity docs): `GET /v4/data/network/NEM?metrics=power&primary_grouping=network_region&secondary_grouping=fueltech`, `Authorization: Bearer <key>`. Response `data[]` elements each have `network_region` (`"VIC"`), `fueltech` (`"solar_utility"`, …), `columns` (`["timestamp","value"]`), and `results` (array of `[timestamp, value]` pairs). We take each series' **last** datapoint as "current" and sum into per-region buckets.

**Files:**
- Create: `core/include/nem/fuel.h`
- Create: `core/src/fuel.c`
- Create: `core/include/nem/oe_client.h`
- Create: `core/src/oe_client.c`
- Create: `core/test/fixtures/oe_power_fueltech.json`
- Modify: `core/CMakeLists.txt` (add `src/fuel.c`, `src/oe_client.c`; `nem_add_test(test_fuel)`, `nem_add_test(test_oe_client)`)
- Test: `core/test/test_fuel.c`, `core/test/test_oe_client.c`

**Interfaces:**
- Consumes: `nem_region_from_short` (Task 1), cJSON.
- Produces (`fuel.h`):
  - `typedef enum { NEM_FUEL_COAL=0, NEM_FUEL_GAS, NEM_FUEL_HYDRO, NEM_FUEL_WIND, NEM_FUEL_SOLAR, NEM_FUEL_BATTERY, NEM_FUEL_OTHER, NEM_FUEL_COUNT } nem_fuel_t;`
  - `nem_fuel_mix_t { double mw[NEM_FUEL_COUNT]; double total_mw; double renewable_fraction; bool valid; }`
  - `nem_region_mix_t { nem_fuel_mix_t regions[NEM_REGION_COUNT]; }`
  - `bool nem_fueltech_map(const char *ft, nem_fuel_t *bucket, bool *renewable, bool *is_load);` → false if `ft` unknown
- Produces (`oe_client.h`):
  - `bool nem_oe_parse_power(const char *json, nem_region_mix_t *out);`

- [ ] **Step 1: Write the failing test `core/test/test_fuel.c`**

```c
#include "unity.h"
#include "nem/fuel.h"

void setUp(void) {}
void tearDown(void) {}

static void test_maps_known(void) {
    nem_fuel_t b; bool ren, load;
    TEST_ASSERT_TRUE(nem_fueltech_map("coal_black", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_COAL, b); TEST_ASSERT_FALSE(ren); TEST_ASSERT_FALSE(load);

    TEST_ASSERT_TRUE(nem_fueltech_map("solar_rooftop", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_SOLAR, b); TEST_ASSERT_TRUE(ren);

    TEST_ASSERT_TRUE(nem_fueltech_map("wind", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_WIND, b); TEST_ASSERT_TRUE(ren);

    TEST_ASSERT_TRUE(nem_fueltech_map("battery_discharging", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_BATTERY, b); TEST_ASSERT_FALSE(load);

    TEST_ASSERT_TRUE(nem_fueltech_map("battery_charging", &b, &ren, &load));
    TEST_ASSERT_TRUE(load);  /* load: excluded from generation totals */

    TEST_ASSERT_TRUE(nem_fueltech_map("gas_ccgt", &b, &ren, &load));
    TEST_ASSERT_EQUAL_INT(NEM_FUEL_GAS, b); TEST_ASSERT_FALSE(ren);
}

static void test_unknown(void) {
    nem_fuel_t b; bool ren, load;
    TEST_ASSERT_FALSE(nem_fueltech_map("unobtainium", &b, &ren, &load));
    TEST_ASSERT_FALSE(nem_fueltech_map(NULL, &b, &ren, &load));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_maps_known);
    RUN_TEST(test_unknown);
    return UNITY_END();
}
```

- [ ] **Step 2: Write `core/include/nem/fuel.h`**

```c
#ifndef NEM_FUEL_H
#define NEM_FUEL_H

#include <stdbool.h>
#include "nem/regions.h"

typedef enum {
    NEM_FUEL_COAL = 0,
    NEM_FUEL_GAS,
    NEM_FUEL_HYDRO,
    NEM_FUEL_WIND,
    NEM_FUEL_SOLAR,
    NEM_FUEL_BATTERY,
    NEM_FUEL_OTHER,
    NEM_FUEL_COUNT
} nem_fuel_t;

typedef struct {
    double mw[NEM_FUEL_COUNT];
    double total_mw;
    double renewable_fraction; /* 0..1 */
    bool   valid;
} nem_fuel_mix_t;

typedef struct {
    nem_fuel_mix_t regions[NEM_REGION_COUNT];
} nem_region_mix_t;

bool nem_fueltech_map(const char *ft, nem_fuel_t *bucket, bool *renewable, bool *is_load);

#endif
```

- [ ] **Step 3: Write `core/src/fuel.c`**

```c
#include "nem/fuel.h"
#include <string.h>

typedef struct { const char *ft; nem_fuel_t bucket; bool renewable; bool is_load; } map_row_t;

static const map_row_t ROWS[] = {
    { "coal_black",          NEM_FUEL_COAL,    false, false },
    { "coal_brown",          NEM_FUEL_COAL,    false, false },
    { "gas_ccgt",            NEM_FUEL_GAS,     false, false },
    { "gas_ocgt",            NEM_FUEL_GAS,     false, false },
    { "gas_recip",           NEM_FUEL_GAS,     false, false },
    { "gas_steam",           NEM_FUEL_GAS,     false, false },
    { "gas_wcmg",            NEM_FUEL_GAS,     false, false },
    { "distillate",          NEM_FUEL_OTHER,   false, false },
    { "bioenergy_biomass",   NEM_FUEL_OTHER,   true,  false },
    { "bioenergy_biogas",    NEM_FUEL_OTHER,   true,  false },
    { "hydro",               NEM_FUEL_HYDRO,   true,  false },
    { "wind",                NEM_FUEL_WIND,    true,  false },
    { "solar_utility",       NEM_FUEL_SOLAR,   true,  false },
    { "solar_rooftop",       NEM_FUEL_SOLAR,   true,  false },
    { "battery_discharging", NEM_FUEL_BATTERY, true,  false },
    { "battery_charging",    NEM_FUEL_BATTERY, false, true  },
    { "pumps",               NEM_FUEL_HYDRO,   false, true  },
};

bool nem_fueltech_map(const char *ft, nem_fuel_t *bucket, bool *renewable, bool *is_load) {
    if (!ft) return false;
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        if (strcmp(ft, ROWS[i].ft) == 0) {
            *bucket = ROWS[i].bucket;
            *renewable = ROWS[i].renewable;
            *is_load = ROWS[i].is_load;
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 4: Wire `fuel` into CMake, run test_fuel (fail → pass)**

Add `src/fuel.c` to `nem_core` and `nem_add_test(test_fuel)`.
Run: `cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build -R test_fuel --output-on-failure`
Expected: passes (write header/src before building; the RED check here is that `test_fuel` links only after `fuel.c` exists — if you build before adding `fuel.c` it fails to link).

- [ ] **Step 5: Write the fixture `core/test/fixtures/oe_power_fueltech.json`**

```json
{
  "version": "4.5.5",
  "success": true,
  "data": [
    { "network_region": "VIC", "fueltech": "coal_brown", "columns": ["timestamp","value"],
      "results": [["2026-07-10T19:20:00Z", 2000.0], ["2026-07-10T19:25:00Z", 1900.0]] },
    { "network_region": "VIC", "fueltech": "wind", "columns": ["timestamp","value"],
      "results": [["2026-07-10T19:25:00Z", 1300.0]] },
    { "network_region": "VIC", "fueltech": "solar_rooftop", "columns": ["timestamp","value"],
      "results": [["2026-07-10T19:25:00Z", 300.0]] },
    { "network_region": "VIC", "fueltech": "battery_charging", "columns": ["timestamp","value"],
      "results": [["2026-07-10T19:25:00Z", 150.0]] },
    { "network_region": "SA",  "fueltech": "wind", "columns": ["timestamp","value"],
      "results": [["2026-07-10T19:25:00Z", 900.0]] }
  ],
  "total_records": 5
}
```

- [ ] **Step 6: Write the failing test `core/test/test_oe_client.c`**

```c
#include "unity.h"
#include "nem/oe_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

static char *read_fixture(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb"); TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f); return b;
}

static void test_parses_vic_mix(void) {
    char *json = read_fixture("oe_power_fueltech.json");
    nem_region_mix_t m;
    TEST_ASSERT_TRUE(nem_oe_parse_power(json, &m));

    const nem_fuel_mix_t *vic = &m.regions[NEM_REGION_VIC];
    TEST_ASSERT_TRUE(vic->valid);
    /* last coal_brown value 1900, wind 1300, solar 300; charging is load -> excluded */
    TEST_ASSERT_EQUAL_DOUBLE(1900.0, vic->mw[NEM_FUEL_COAL]);
    TEST_ASSERT_EQUAL_DOUBLE(1300.0, vic->mw[NEM_FUEL_WIND]);
    TEST_ASSERT_EQUAL_DOUBLE(300.0,  vic->mw[NEM_FUEL_SOLAR]);
    TEST_ASSERT_EQUAL_DOUBLE(3500.0, vic->total_mw);
    /* renewable = (1300+300)/3500 */
    TEST_ASSERT_TRUE(fabs(vic->renewable_fraction - (1600.0/3500.0)) < 1e-9);

    const nem_fuel_mix_t *sa = &m.regions[NEM_REGION_SA];
    TEST_ASSERT_TRUE(sa->valid);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, sa->renewable_fraction); /* all wind */
    free(json);
}

static void test_rejects_garbage(void) {
    nem_region_mix_t m;
    TEST_ASSERT_FALSE(nem_oe_parse_power("{}", &m));
    TEST_ASSERT_FALSE(nem_oe_parse_power(NULL, &m));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_vic_mix);
    RUN_TEST(test_rejects_garbage);
    return UNITY_END();
}
```

- [ ] **Step 7: Write `core/include/nem/oe_client.h`**

```c
#ifndef NEM_OE_CLIENT_H
#define NEM_OE_CLIENT_H

#include <stdbool.h>
#include "nem/fuel.h"

bool nem_oe_parse_power(const char *json, nem_region_mix_t *out);

#endif
```

- [ ] **Step 8: Wire into CMake and run to verify failure**

Add `src/oe_client.c` to `nem_core` and `nem_add_test(test_oe_client)`.
Run: `cmake -S core -B core/build && cmake --build core/build`
Expected: failure — `src/oe_client.c` missing.

- [ ] **Step 9: Write `core/src/oe_client.c`**

```c
#include "nem/oe_client.h"
#include "nem/regions.h"
#include "cJSON.h"
#include <string.h>

/* Return the last datapoint value in a results array ([ts, value] pairs). */
static bool last_value(const cJSON *results, double *out) {
    if (!cJSON_IsArray(results)) return false;
    int n = cJSON_GetArraySize(results);
    if (n == 0) return false;
    const cJSON *pair = cJSON_GetArrayItem(results, n - 1);
    if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) < 2) return false;
    const cJSON *v = cJSON_GetArrayItem(pair, 1);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

bool nem_oe_parse_power(const char *json, nem_region_mix_t *out) {
    memset(out, 0, sizeof(*out));
    if (!json) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) { cJSON_Delete(root); return false; }

    /* Accumulate renewable MW separately to compute the fraction. */
    double renewable_mw[NEM_REGION_COUNT] = {0};
    int series = 0;

    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, data) {
        const cJSON *reg = cJSON_GetObjectItemCaseSensitive(el, "network_region");
        const cJSON *ft  = cJSON_GetObjectItemCaseSensitive(el, "fueltech");
        if (!cJSON_IsString(reg) || !cJSON_IsString(ft)) continue;
        nem_region_t r = nem_region_from_short(reg->valuestring);
        if (r >= NEM_REGION_COUNT) continue;

        nem_fuel_t bucket; bool renewable, is_load;
        if (!nem_fueltech_map(ft->valuestring, &bucket, &renewable, &is_load)) continue;
        if (is_load) continue;

        double v = 0.0;
        if (!last_value(cJSON_GetObjectItemCaseSensitive(el, "results"), &v)) continue;

        nem_fuel_mix_t *m = &out->regions[r];
        m->mw[bucket] += v;
        m->total_mw += v;
        if (renewable) renewable_mw[r] += v;
        m->valid = true;
        series++;
    }

    for (int r = 0; r < NEM_REGION_COUNT; r++) {
        nem_fuel_mix_t *m = &out->regions[r];
        m->renewable_fraction = (m->total_mw > 0.0) ? renewable_mw[r] / m->total_mw : 0.0;
    }

    cJSON_Delete(root);
    return series > 0;
}
```

- [ ] **Step 10: Build and run to verify pass**

Run: `cmake --build core/build && ctest --test-dir core/build --output-on-failure`
Expected: `test_fuel` and `test_oe_client` pass (all suites green).

- [ ] **Step 11: Commit**

```bash
git add core/
git commit -m "feat(core): parse OpenElectricity generation mix by fueltech"
```

---

## Task 7: `alerts` — event evaluation engine

Emits an event only on the transition *into* an alert condition (debounce); while the condition persists there is no re-emit; when it clears, the flag resets so the next occurrence fires again. Renewable alerts require a fuel mix (may be NULL → skipped). Chime suppression is computed by the caller via `nem_config_should_chime`.

**Files:**
- Create: `core/include/nem/alerts.h`
- Create: `core/src/alerts.c`
- Modify: `core/CMakeLists.txt` (add `src/alerts.c`; `nem_add_test(test_alerts)`)
- Test: `core/test/test_alerts.c`

**Interfaces:**
- Consumes: `nem_snapshot_t` (Task 2), `nem_config_t`/`nem_thresholds_t` (Task 2), `nem_region_mix_t` (Task 6).
- Produces:
  - `typedef enum { NEM_ALERT_NONE=0, NEM_ALERT_SPIKE, NEM_ALERT_EXTREME_SPIKE, NEM_ALERT_NEGATIVE, NEM_ALERT_HIGH_DEMAND, NEM_ALERT_HIGH_RENEWABLE, NEM_ALERT_TYPE_COUNT } nem_alert_type_t;`
  - `nem_alert_event_t { nem_alert_type_t type; nem_region_t region; double value; }`
  - `nem_alert_state_t { bool active[NEM_REGION_COUNT][NEM_ALERT_TYPE_COUNT]; }`
  - `void nem_alerts_init(nem_alert_state_t *st);`
  - `int nem_alerts_evaluate(nem_alert_state_t *st, const nem_config_t *cfg, const nem_snapshot_t *snap, const nem_region_mix_t *mix, nem_alert_event_t *events, int max_events);`

- [ ] **Step 1: Write the failing test `core/test/test_alerts.c`**

```c
#include "unity.h"
#include "nem/alerts.h"
#include "nem/config.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static nem_snapshot_t snap_with(nem_region_t r, double price, double demand) {
    nem_snapshot_t s; memset(&s, 0, sizeof(s));
    for (int i = 0; i < NEM_REGION_COUNT; i++) s.regions[i].region = (nem_region_t)i;
    s.regions[r].valid = true;
    s.regions[r].price = price;
    s.regions[r].demand_mw = demand;
    return s;
}

static void test_spike_fires_once_then_debounces(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];

    nem_snapshot_t s = snap_with(NEM_REGION_VIC, 450.0, 6000.0); /* > 300, < 1000 */
    int n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_SPIKE, ev[0].type);
    TEST_ASSERT_EQUAL_INT(NEM_REGION_VIC, ev[0].region);

    /* Still spiking -> no new event. */
    n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(0, n);

    /* Clears, then spikes again -> fires again. */
    nem_snapshot_t calm = snap_with(NEM_REGION_VIC, 90.0, 6000.0);
    nem_alerts_evaluate(&st, &cfg, &calm, NULL, ev, 8);
    n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
}

static void test_extreme_supersedes_spike(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t s = snap_with(NEM_REGION_VIC, 9420.0, 6000.0);
    int n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_EXTREME_SPIKE, ev[0].type);
}

static void test_negative_price(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t s = snap_with(NEM_REGION_SA, -18.0, 1200.0);
    int n = nem_alerts_evaluate(&st, &cfg, &s, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_NEGATIVE, ev[0].type);
}

static void test_high_demand_uses_configured_mark(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    cfg.thresholds.high_demand_mw[NEM_REGION_NSW] = 12000.0;
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t under = snap_with(NEM_REGION_NSW, 100.0, 11000.0);
    TEST_ASSERT_EQUAL_INT(0, nem_alerts_evaluate(&st, &cfg, &under, NULL, ev, 8));
    nem_snapshot_t over = snap_with(NEM_REGION_NSW, 100.0, 12500.0);
    int n = nem_alerts_evaluate(&st, &cfg, &over, NULL, ev, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(NEM_ALERT_HIGH_DEMAND, ev[0].type);
}

static void test_high_renewable_needs_mix(void) {
    nem_config_t cfg; nem_config_defaults(&cfg);
    nem_alert_state_t st; nem_alerts_init(&st);
    nem_alert_event_t ev[8];
    nem_snapshot_t s = snap_with(NEM_REGION_SA, 40.0, 1200.0);

    nem_region_mix_t mix; memset(&mix, 0, sizeof(mix));
    mix.regions[NEM_REGION_SA].valid = true;
    mix.regions[NEM_REGION_SA].renewable_fraction = 0.92;

    int n = nem_alerts_evaluate(&st, &cfg, &s, &mix, ev, 8);
    /* SA renewable event present among results */
    bool found = false;
    for (int i = 0; i < n; i++)
        if (ev[i].type == NEM_ALERT_HIGH_RENEWABLE && ev[i].region == NEM_REGION_SA) found = true;
    TEST_ASSERT_TRUE(found);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_spike_fires_once_then_debounces);
    RUN_TEST(test_extreme_supersedes_spike);
    RUN_TEST(test_negative_price);
    RUN_TEST(test_high_demand_uses_configured_mark);
    RUN_TEST(test_high_renewable_needs_mix);
    return UNITY_END();
}
```

- [ ] **Step 2: Write `core/include/nem/alerts.h`**

```c
#ifndef NEM_ALERTS_H
#define NEM_ALERTS_H

#include <stdbool.h>
#include "nem/snapshot.h"
#include "nem/config.h"
#include "nem/fuel.h"

typedef enum {
    NEM_ALERT_NONE = 0,
    NEM_ALERT_SPIKE,
    NEM_ALERT_EXTREME_SPIKE,
    NEM_ALERT_NEGATIVE,
    NEM_ALERT_HIGH_DEMAND,
    NEM_ALERT_HIGH_RENEWABLE,
    NEM_ALERT_TYPE_COUNT
} nem_alert_type_t;

typedef struct {
    nem_alert_type_t type;
    nem_region_t     region;
    double           value;
} nem_alert_event_t;

typedef struct {
    bool active[NEM_REGION_COUNT][NEM_ALERT_TYPE_COUNT];
} nem_alert_state_t;

void nem_alerts_init(nem_alert_state_t *st);
int  nem_alerts_evaluate(nem_alert_state_t *st, const nem_config_t *cfg,
                         const nem_snapshot_t *snap, const nem_region_mix_t *mix,
                         nem_alert_event_t *events, int max_events);

#endif
```

- [ ] **Step 3: Wire into CMake and run to verify failure**

Add `src/alerts.c` to `nem_core` and `nem_add_test(test_alerts)`.
Run: `cmake -S core -B core/build && cmake --build core/build`
Expected: failure — `src/alerts.c` missing.

- [ ] **Step 4: Write `core/src/alerts.c`**

```c
#include "nem/alerts.h"

void nem_alerts_init(nem_alert_state_t *st) {
    for (int r = 0; r < NEM_REGION_COUNT; r++)
        for (int t = 0; t < NEM_ALERT_TYPE_COUNT; t++)
            st->active[r][t] = false;
}

/* Emit on rising edge only: fire when condition true and not already active. */
static int edge(nem_alert_state_t *st, int r, nem_alert_type_t t, bool cond,
                double value, nem_alert_event_t *events, int max, int n) {
    if (cond) {
        if (!st->active[r][t]) {
            st->active[r][t] = true;
            if (n < max) {
                events[n].type = t;
                events[n].region = (nem_region_t)r;
                events[n].value = value;
                return n + 1;
            }
        }
    } else {
        st->active[r][t] = false;
    }
    return n;
}

int nem_alerts_evaluate(nem_alert_state_t *st, const nem_config_t *cfg,
                        const nem_snapshot_t *snap, const nem_region_mix_t *mix,
                        nem_alert_event_t *events, int max_events) {
    const nem_thresholds_t *th = &cfg->thresholds;
    int n = 0;

    for (int r = 0; r < NEM_REGION_COUNT; r++) {
        const nem_region_snapshot_t *rs = &snap->regions[r];
        if (!rs->valid) continue;

        bool extreme = rs->price > th->extreme_spike_price;
        bool spike   = rs->price > th->spike_price && !extreme; /* extreme supersedes */
        n = edge(st, r, NEM_ALERT_EXTREME_SPIKE, extreme, rs->price, events, max_events, n);
        n = edge(st, r, NEM_ALERT_SPIKE,         spike,   rs->price, events, max_events, n);

        bool negative = rs->price < th->negative_price;
        n = edge(st, r, NEM_ALERT_NEGATIVE, negative, rs->price, events, max_events, n);

        bool high_demand = th->high_demand_mw[r] > 0.0 && rs->demand_mw > th->high_demand_mw[r];
        n = edge(st, r, NEM_ALERT_HIGH_DEMAND, high_demand, rs->demand_mw, events, max_events, n);

        bool high_ren = false; double ren_val = 0.0;
        if (mix && mix->regions[r].valid) {
            ren_val = mix->regions[r].renewable_fraction;
            high_ren = ren_val > th->high_renewable_frac;
        }
        n = edge(st, r, NEM_ALERT_HIGH_RENEWABLE, high_ren, ren_val, events, max_events, n);
    }
    return n;
}
```

- [ ] **Step 5: Build and run the full suite**

Run: `cmake --build core/build && ctest --test-dir core/build --output-on-failure`
Expected: **all eight test suites pass**, `100% tests passed`.

- [ ] **Step 6: Commit**

```bash
git add core/
git commit -m "feat(core): add alert evaluation engine with edge-triggered debounce"
```

---

## Self-Review

**Spec coverage (against `2026-07-10-nem-buddy-realtime-monitor-design.md`):**
- §4 data source / AEMO summary parsing → Task 4 ✓
- §4 self-accumulated intraday history → Task 5 ✓
- §4 generation mix (OpenElectricity) → Task 6 ✓
- §4 alert thresholds + debounce + quiet hours → Tasks 2 (quiet/chime) + 7 (thresholds/debounce) ✓
- §5 `config`, `nem_client` (split into `aemo_client`/`oe_client`), `alerts` as isolated units → Tasks 2/4/6/7 ✓
- §5 `store` intraday buffers → Task 5 ✓
- Interconnector flows (§6 drill screen 3) → parsed in Task 4 ✓
- **Deferred to later plans (correctly out of this library's scope):** `board`, `provisioning`, `ui`, `audio`, NVS persistence of `config`, and the runtime HTTP fetch/poll timers. This plan is the pure logic core only.

**Placeholder scan:** No TBD/TODO; every code step contains complete, compilable code; every test step names a concrete command and expected result. ✓

**Type consistency:** `nem_region_t`, `nem_snapshot_t`, `nem_region_snapshot_t`, `nem_fuel_mix_t`/`nem_region_mix_t`, `nem_config_t`, `nem_alert_event_t` are defined once (Tasks 1/2/6/7) and consumed with matching signatures downstream. `nem_region_from_short` (defined Task 1) is used in Task 6. `nem_minute_of_day`/`nem_day_index` (Task 3) used in Task 5. ✓

**Note on AEMO high-demand marks:** defaults are `0` (disabled) so high-demand alerts don't fire until the user sets a per-region mark in Settings (a later plan). This matches the Global Constraints.
