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
6. **Sleep state**: battery sampling 50× slower. (The optional `SLEEP_CYW43_OFF`
   radio power-down from this round was never bench-tested and is superseded by the
   round-2 deep nap below.)

---

## Round 2 (2026-11-16) — why it still only lasted a day

Field report: _"still not getting accurate battery reporting, and the device barely
lasts a day, two at the most."_ A unit that had been up for 20 h on the round-1
firmware was put on USB serial and showed the cause directly.

### What was actually wrong

1. **The radio chip hangs, and the battery sampler then pins a core.** The log read
   `[CYW43] STALL(0;130-130): timeout` once a second, forever. The CYW43 had stopped
   granting bus credits, and the driver's reaction to that is a **one-second busy-wait
   with the radio lock held** for every ioctl. The sampler issued an ioctl twice a
   second (the VBUS pin), so core 0 was spinning flat out around the clock. A fresh
   reboot of the same firmware shows no stall — it sets in during uptime, and nothing
   ever cleared it.
2. **The same hang broke USB detection, and with it the gauge.** With the ioctl
   failing, USB read as "not present": on the charger the gauge treated the 4.43 V USB
   rail as a cell (`raw 4.428 V + 530 mV → 4.96 V = 100%`), and the auto-anchor — which
   needs to see USB arrive and leave — could never fire.
3. **Probable origin of the hang: the radio's bus timing does not work at 48 MHz.**
   The SDK's default PIO SPI program for the CYW43 (`spi_gap01_sample0`) is "for high
   cpu speed". Booted at 48 MHz the chip does not start at all (`[CYW43] Failed to
   start CYW43`, captured on the bench); with the SDK's low-speed variant
   (`spi_gap0_sample1`) it starts and runs normally. Round 1's sleep state dropped to
   48 MHz with the chip still live on the high-speed program.
4. **Polling the chip kept it awake.** Even healthy, an ioctl every 500 ms never lets
   the CYW43's bus go idle (the driver sleeps it after ~2.5 s of quiet).
5. **Awake was simply expensive.** 150 MHz for an e-ink UI, and Greg is awake whenever
   he is being carried (the accelerometer keeps him up for the pedometer).

### What changed (verified on a unit over USB serial unless marked)

| # | Change | Where |
|---|---|---|
| 1 | **Runs at 48 MHz all the time** off PLL_USB with PLL_SYS powered down, set first thing in `main()` so every baud rate and the RTOS tick size themselves from it. The clock is no longer switched under a live radio. | `power.c` `power_boot_begin` |
| 2 | **Radio bus program for 48 MHz**: `CYW43_SPI_PROGRAM_NAME=spi_gap0_sample1`. | `CMakeLists.txt` |
| 3 | **USB presence comes from the VSYS reading** (≥ 4.10 V is USB — a diode-fed cell tops out near 3.9 V; ≤ 4.00 V drops back), not from the radio's answer. | `battery.c` `battery_usb_from_raw` |
| 4 | **Battery sampled every 10 s awake / 30 s napping instead of 2 Hz.** The read still needs one ioctl first (without a transaction the ADC sees ~0.03 V), so the cadence is what lets the radio chip sleep between samples. | `power.c`, `battery.c` |
| 5 | **Radio health check**: that same ioctl's result is kept; three failed samples in a row → a **warm restart** (steps, mood and clock carried over, no splash) that re-initialises the chip. At most one automatic restart per cold boot. | `power.c` `radio_health_check`, `power_warm_restart` |
| 6 | **Readings below 2.5 V are rejected** as failed reads — they can no longer flip the USB state or trip the critical-battery nap. | `power.c` `batt_sample` |
| 7 | **Unattended redraws ÷ 4**: no press for 3 min while being carried → one panel refresh per quote (~12 s) instead of the four-frame mouth animation. Each refresh measures ~750 ms. _(not exercised on the bench)_ | `app_main.c` `STATE_OCTOPUS` |
| 8 | **Gauge only falls on battery** (a rise must be ≥ 4 points and persist). _(not exercised: the bench unit was on USB throughout)_ | `power.c` `gauge_monotonic` |
| 9 | **Offset re-based for the lower draw**: default 530 → 450 mV; an offset anchored under the old firmware is pulled down 80 mV once. An estimate — the next anchor makes it exact. | `battery.h`, `storage.c` |

### Tried and backed out

- **Lower core voltage (1.00 V).** The regulator setting survives a warm reset while the
  boot ROM and C runtime start at 150 MHz; the next warm boot froze and could not even
  enumerate in BOOTSEL. The rail now stays at the default and is restored at boot.
- **Waking / sleeping the radio bus directly** instead of the ioctl: froze the firmware
  (it ran against a radio that had failed to start). Removed.
- **Deep nap** (power the RP2350's switched core off via POWMAN; CENTER or a timed
  alarm wakes it; state carried in the POWMAN scratch registers and the always-on
  timer). For most of the day this looked dead: the unit went down and came back on
  neither CENTER nor the alarm, and needed a full power removal every time. With a
  second Pico 2 W flashed as a debug probe on the SWD pads the cause turned out to be
  our own error handling. The SDK's `powman_set_power_state()` polls for the
  "waiting for the processors to sleep" flag for only ~500 loop iterations (a few
  microseconds) and returns a TIMEOUT error while the request is still pending. The
  firmware took that for a refusal, **disarmed both wake sources and cleared the nap
  record — and then the chip powered down anyway**, with nothing left to wake it.
  `deep_power_down()` now issues the request itself, waits properly, and withdraws the
  request on a real refusal. Verified with the probe attached and the unit on USB:
  power-down, self-wake on the alarm after 20 s and after 10 min (`slept 0h10m`, clock
  carried through), classification as a quiet USB peek, and wake from there.
  Also verified (2026-11-16 evening): a physical CENTER press wakes it (`last_pwrup=02`,
  30 s into a nap, well before the alarm); and on battery the timed peek finds no USB
  and powers straight back down (unplugged for 5 min, replugged: the unit stayed off
  USB until its next 15-minute peek, then reported `slept 0h16m` as a quiet USB peek).
  Note: `NAP DEEP <s>` sets only the first peek; later peeks use `DEEP_NAP_PEEK_S`.
  **First overnight run (2026-11-16/17) was invalid — bench error.** The unit was
  flashed over SWD and the debugger's power-up request was not cleared afterwards, so
  POWMAN refused every power-down. The refusal path then cold-booted: chime, redraw,
  doze off, refused, reboot — 128 boots in ~11 h (the gauge still showed 89 %). Fixed
  in firmware: a refused power-down now reboots QUIETLY into the light nap
  (`REFUSED_MAGIC`, logged as `[NAP] power-down REFUSED`) and retries one peek period
  later. Bench rule: after any SWD flash, run `rp2350.dap dpreg 4 0` (or power-cycle
  the probe/target) before a battery test. Units without a probe cannot hit this.
  **Second run (2026-11-17/18, unit A025, no probe): ~24 h on battery, deep nap held
  the whole time** (no reboots, woke itself on the USB peek, manual anchor kept). Two
  defects found:
  - *Clock lost 13 min in the day.* The always-on timer runs on the LPOSC through a
    nap; this chip's factory figure is 32 660 Hz and its LPOSC measured 32 451..32 536 Hz
    awake (-0.4..-0.6 %), slower again asleep. `lposc_measure_hz()` now times 400 timer ticks against the
    crystal before every power-down and trims POWMAN to the result. The second unit
    (Sweaty Moth, LPOSC ~26.6 kHz with a factory figure of 26 573) lost 6.5 min in
    ~25 h untrimmed. What is left after the trim, over single 8-minute naps on USB:
    -0.58 % on A025; -0.16 % and +0.12 % on Sweaty Moth. The LPOSC is an RC
    oscillator whose reading wanders a few tenths of a percent within minutes
    (26 513..26 849 Hz in half an hour), so one reading cannot do better.
    Tried and backed out: a fixed "slower asleep" factor (-4800 ppm fitted A025 to
    0.01 %, made Sweaty Moth 0.49 % fast) and correcting the nap afterwards from a
    second reading at the wake (turned a +0.12 % nap into -0.19 %).
    Expect a few minutes a day at worst; a full day of 15-minute peeks re-trims 96
    times and should average better than a single nap, but that is unmeasured. The
    WiFi time sync removes the rest. Bench: `LPOSC?` measures the oscillator now.
  - *The battery % at lights-out could not be read after the wake* (printed only in
    the boot banner). `PWR?` now adds a `NAPINFO lposc before nap=… at wake=… cell_at_nap=…%` line, an
    on-battery peek refreshes the stored % with its own reading, and the periodic
    `[BATT] USB rail … last on-battery …` line carries it.
  The % for this run was therefore lost; AC11 is still open.
  **Not yet verified: a multi-day run on battery and many nap/wake cycles in a row.**
  Until then it stays behind `-DDEEP_NAP=ON`, off by default, and the shipped nap is
  the light nap (radios and panel off, 48 MHz WFI).

Build switches: `-DAWAKE_CLK_48MHZ=OFF`, `-DDEEP_NAP=ON` (experimental),
`-DBOOT_TRACE=1` / `-DBOOT_LOG_DELAY_MS=<ms>` (bench: boot markers, and a pause so a
serial monitor can attach). The old `SLEEP_CYW43_OFF` option is gone.

### Bench commands (USB serial, 115200)

| Command | Effect |
|---|---|
| `PWR?` | clocks, boot kind, USB verdict, gauge, offset |
| `KEY U\|D\|L\|R\|C` | inject a joystick press |
| `NAP` | doze off now |
| `RESTART` | the warm restart the radio health check performs |
| `NAP DEEP [s]` | experimental builds only: force the deep nap |

Useful log lines: `[PWR] boot: …`, `[CYW43] radio did not answer (n/3)`,
`[BATT] invalid VSYS read … — ignored`.

### Still unknown

No current has been measured. The expected gain is "a hung radio no longer spins a
core, and awake runs at a third of the clock"; how many days that buys needs a
multi-day run on battery, and the measurement table below is still empty.

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
- [ ] **AC12 (hardware):** a Greg left awake for 24 h shows no `[CYW43] STALL` lines; if the radio does hang, the health
      check restarts it within ~30 s.
- [ ] **AC13 (hardware):** an overnight charge auto-anchors on unplug (USB is now detected from VSYS).
- [ ] **AC14 (hardware):** deep nap (`-DDEEP_NAP=ON`): wakes on a physical CENTER press; on battery the timed USB peek goes
      straight back down; 20 nap/wake cycles in a row. (Alarm wake, the USB-present peek, one CENTER wake and one
      on-battery peek pass on the bench; the 20-cycle and multi-day runs are outstanding.)

### Measurement table (fill in)

| State | Expected (mA) | Measured (mA) |
|---|---|---|
| Awake, screen animating every 3 s, radios off | 8–12 | |
| Awake, screen frozen (30 s no motion) | 6–9 | |
| + social scanning (5 % profile) | +1.5–2.5 | |
| + WiFi ALWAYS ON (PM1) | +2–3 | |
| Nap (light: radios + panel off, 48 MHz) | 4–7 | |
| Nap (deep, `-DDEEP_NAP=ON`) | ~0.3 | |

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
- Build flags: `BATT_DIRECT_ADC_GPIO`, `BATT_DIRECT_DIVIDER`, `BATT_OFFSET_DEFAULT_MV`, `AWAKE_CLK_48MHZ`, `DEEP_NAP`, `DEEP_NAP_PEEK_S`
- Serial diagnostics: `[BATT] raw … + …mV → … V = …%  left … h`, `[BATT] auto-anchor …`, `[WiFi] sync done — dropping the link`
- Related: [Off-State Screen](../complete/Feature_OffStateScreen_Status-Complete.md) (sleep state), Issue #4 (power switch)
