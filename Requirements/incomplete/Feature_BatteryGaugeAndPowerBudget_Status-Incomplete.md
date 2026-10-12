# Feature: Accurate Battery Gauge + Power Budget (multi-day battery life)

**Status:** ❌ Incomplete (firmware implemented, awaiting hardware verification)
**Branch:** `feature/battery-gauge-power-budget`
**Source:** Field report (2026-10-10) — _"battery level is inaccurate; a full charge rarely lasts a day"_
**Owner:** rompasaurus
**Priority:** High
**Type:** Power management / measurement

---

## User Story

> **As a** WetGreg owner,
> **I want** the battery icon and percentage to be believable, a sense of how long
> Greg has left, and a full charge to last several days,
> **so that** Greg isn't dead in my pocket by the evening and I'm not charging him blind.

---

## What the hardware allows (see `Dilder-PCB/.../POWER_PATH_NOTES.md`)

The Rev 1 board has **no direct cell sense**. The firmware reads the Pico's own
VSYS/3 divider, and on battery VSYS is the cell **minus the drop across D3 (a BAT54
Schottky)** — 0.3 V at light load, up to 0.8 V during radio bursts, different on
every board and with temperature. On USB the cell is invisible (D3 reverse-biased;
the Pico runs off VBUS, and the TP4056 charges the cell at its full ~100 mA
regardless of what the firmware does — the old "charging power-diet" was based on a
wrong assumption and has been removed).

The old gauge added a fixed 0.53 V. That constant was measured with both cores
spinning (~25–30 mA) and is wrong at any other load; it is also the single largest
reason the reading drifted between boards and sessions.

---

## What was built

### Gauge (drivers/battery.c, sys/power.c, sys/storage.c)
- **One persisted additive offset per board** (`g_saved.batt_offset_mv`, default
  530 mV, the value measured on this board) replaces the multiplicative "CAL" trim, which was never persisted.
- **Auto-anchor**: on every USB unplug that follows a long enough charge (from the
  level the cell had at plug-in to the CV phase at 100 mA, plus 1 h; ~11 h from
  empty, ~3 h from 70 %), the first clean reading is anchored to 4.19 V (the TP4056's CV float minus relaxation), slew-limited
  to 60 mV per event. Two or three charge cycles converge any board. Device Info →
  UP anchors by hand (right after a full charge), DOWN resets to the default.
- **Peak-hold window** (4 s awake / 40 s asleep) keeps the lightest-load sample so
  the offset is defined at the quiet baseline; EMA on top. Sampling 2 Hz awake,
  0.2 Hz asleep (each read briefly holds the cyw43 lock).
- **Time left**: a measured %/hour rate (taken only across ≥ 4-point drops ≥ 10 min
  apart, then EMA'd) gives "~18H"; before any rate exists a nominal-draw estimate is
  shown with a `*` ("~60H*"). Shown on Device Info; `[BATT]` serial lines log it.
- **USB detection without the radio**: VSYS ≥ 4.45 V ⇒ USB (a diode-fed LiPo can't
  get there). Used when the CYW43 is powered down.
- **Direct-sense build option** for the Rev 1 bodge / Rev 2 board:
  `-DBATT_DIRECT_ADC_GPIO=26` reads a 1 MΩ/1 MΩ cell divider on ADC0 instead.
- **Low-battery policy**: ≤ 15 % → BLE proximity scan off, no background WiFi
  (restored at ≥ 22 % or on USB). ≤ 3.40 V → forced sleep with a
  "LOW BATTERY – PLEASE CHARGE ME" footer; CENTER wakes Greg for 15 s to read it.
  The TP4056 has no load cut-off and the Pico's buck-boost runs to ~1.8 V, so this
  is the only thing keeping the cell out of deep discharge.

### Power budget (ordered by estimated impact)
1. **Idle cores now WFI** (`freertos_hooks.c`, `FreeRTOSConfig.h`). FreeRTOS's idle
   task was a busy loop on BOTH cores, ~95 % of the time, at 150 MHz (and still
   spinning at 48 MHz in "sleep"). Single largest item, zero behavioural change.
2. **BLE duty** (`bt.c`): adverts 30 ms → 250 ms, scan 15 % → 5 % (64 ms / 1280 ms).
   Expected discovery of a passing Greg ≈ 5 s. Fast adverts return automatically for
   the 15 s emote burst and while the Bluetooth / SCAN NEARBY screens are open
   (SCAN NEARBY also runs a 50 % scan for sub-second results).
3. **WiFi "SYNC ONLY" mode** (`wifi.c`, Settings → Network → MODE, default): any join
   is dropped 20 s after the NTP reply (90 s for a menu join so the status screen can
   be read), and a background join + sync repeats once a day using the last network
   (2 h retry on failure, never while low on battery). "ALWAYS ON" keeps the link up
   in PM1. The status screen shows the mode and "SYNCED xH AGO".
4. **Identical frames never hit the glass** (`display.c`, `rtos_tasks.c`): every menu
   re-rendered itself on its 4 s poll timeout → a booster-on partial refresh each
   time. `display_render()` now compares against the last frame sent.
5. **Menus time out** (`app_main.c`): 90 s without a press returns to the octopus
   screen (3 min on the password keyboard, 2 min on SCAN NEARBY), from where the
   5-minute stillness nap can happen. Previously a Greg left in a menu never slept.
6. **Sleep state**: battery sampling 50× slower; optional `-DSLEEP_CYW43_OFF=ON`
   powers the radio chip down entirely (GPIO 25 is held high so the VSYS divider
   stays readable). Off by default until bench-tested — it takes the cyw43 arch and
   BTstack through a deinit/init cycle on every nap.

---

## Acceptance Criteria

- [x] **AC1:** Battery offset is persisted and survives reboot/OTA.
- [x] **AC2:** Auto-anchor fires only after a charge long enough to reach CV, moves ≤ 60 mV per event, logs `[BATT] auto-anchor`.
- [x] **AC3:** Device Info shows %, filtered volts, time left, raw volts, offset + source (D/A/M).
- [x] **AC4:** Low (≤ 15 %) suspends the social scan; critical (≤ 3.40 V) forces the CHARGE ME nap.
- [x] **AC5:** Both idle tasks execute WFI; firmware builds and the 1 kHz tick still schedules.
- [x] **AC6:** No panel refresh for a pixel-identical frame; the panel still refreshes after a wake.
- [x] **AC7:** Any sub-screen returns to the octopus after its idle allowance.
- [x] **AC8:** WiFi SYNC ONLY drops the link after NTP; ALWAYS ON keeps it in PM1; mode persisted.
- [ ] **AC9 (hardware):** measured current, battery path (bench meter in series with CN1), in each state:
      awake viewed / awake idle (screen frozen) / asleep / social on / WiFi ALWAYS. Fill in the table below.
- [ ] **AC10 (hardware):** gauge within ±5 % of a meter at 4.1 V, 3.8 V and 3.6 V after two auto-anchor cycles.
- [ ] **AC11 (hardware):** a full charge lasts ≥ 3 days of normal (mixed) use.
- [ ] **AC12 (hardware):** with `SLEEP_CYW43_OFF=ON`, 20 consecutive nap/wake cycles with social on + WiFi ALWAYS, no hang
      (serial attached; the 5 s CENTER-hold watchdog reboot is the escape hatch).

### Measurement table (fill in)

| State | Expected (mA) | Measured (mA) |
|---|---|---|
| Awake, screen animating every 3 s, radios off | 8–12 | |
| Awake, screen frozen (30 s no motion) | 6–9 | |
| + social scanning (5 % profile) | +1.5–2.5 | |
| + WiFi ALWAYS ON (PM1) | +2–3 | |
| Asleep (default) | 2–4 | |
| Asleep (`SLEEP_CYW43_OFF=ON`) | 1–2 | |

---

## Remaining work / future ideas

- [ ] Hardware verification pass (AC9–AC12) and tune the defaults from the numbers:
      `BATT_OFFSET_DEFAULT_MV`, `BATT_NOMINAL_AVG_MA`, `BATT_CAPACITY_MAH` (power.h / battery.h).
- [ ] If the bodge/Rev 2 direct sense lands, flip the default build to `BATT_DIRECT_ADC_GPIO=26`.
- [ ] Rev 2: STDBY/CHRG on GPIO 27/28 → exact "charge complete" anchor + CHARGING/FULL on the icon.
- [ ] SC7A20 INT1 (GP15) motion-wake + true dormant sleep (needs the task split reworked).
- [ ] Drop the USB stdio stack / PLL_USB on battery (~1 mA) — the ADC clock also hangs off PLL_USB, so this needs care.

---

## Notes / References
- Hardware analysis + Rev 2 change list: `Dilder-PCB/Dilder Full PCB Rev 1/POWER_PATH_NOTES.md`
- Build flags: `BATT_DIRECT_ADC_GPIO`, `BATT_DIRECT_DIVIDER`, `BATT_OFFSET_DEFAULT_MV`, `SLEEP_CYW43_OFF`
- Serial diagnostics: `[BATT] raw … + …mV → … V = …%  left … h`, `[BATT] auto-anchor …`, `[WiFi] sync done — dropping the link`
- Related: [Off-State Screen](../complete/Feature_OffStateScreen_Status-Complete.md) (sleep state), Issue #4 (power switch)
