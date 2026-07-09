# Feature: Visible "Off" State Indication (pseudo-off sleep state)

**Status:** ✅ Complete
**Branch:** `feature/off-state-screen`
**Source:** GitHub Issue [#3](https://github.com/rompasaurus/WetGregFirmware/issues/3) — _"Screen clear to denote off state"_ (reporter: daniel-warner-eucom)
**Owner:** rompasaurus
**Priority:** Medium
**Type:** UX / power management

---

## User Story

> **As a** user turning the unit off,
> **I want** the screen to visibly show that it is off,
> **so that** I'm not confused about whether it's still on ("otherwise… shenanigans will ensue").

---

## What was built — STATE_SLEEP, a pseudo-off state

Since the hardware power switch is a hard rail cut (Issue #4) that firmware can't
intercept, the "off" state is implemented as a **firmware pseudo-off**: a sleep
state that is unambiguous on the glass and takes the electronics as close to off
as possible while keeping the wake button alive.

### Entry (either one)
- **5 minutes with no significant motion** (accelerometer-based; button presses
  also reset the timer). Devices without a working accel never auto-sleep.
- **5 CENTER presses within 5 seconds** on the main (octopus) screen — the
  deliberate "turn it off now" gesture.

### The sleep screen
One still frame, orientation-aware (wide + tall layouts):
- Bold title at the top: **"GREG IS SLEEPING"** with **"SHHHHH...."** beneath it
- Greg asleep — closed eyes, tiny snore mouth, and an old-cartoon **nightcap**
  (white band + striped drooping cone + pom-pom)
- **Z's** rising toward the top right in growing sizes
- Rising **bubbles** in the background, like the boot splash (text bands kept
  bubble-free so the still never looks glitched)
- Small text at the bottom: **"PRESS C TO WAKE HIM"**

### Power measures while asleep (`power_sleep_enter` in main.c)
| Subsystem | Action |
|---|---|
| BLE (social + phone) | scan/advertising stopped, **HCI powered off** |
| WiFi | disassociated (STA down); CYW43 chip stays up — VSYS battery sense needs it |
| e-ink panel | **deep sleep** after the still lands (image retained at ~0 power) |
| CPU clock | **150 MHz → 48 MHz** from PLL_USB; **PLL_SYS powered down** (USB serial survives) |
| Core rail | vreg **1.10 V → 1.00 V** |
| Tasks | FreeRTOS ticks stretch ~3× at 48 MHz, so all polling slows proportionally |

### Wake
**CENTER only** (motion does not wake him). Wake restores voltage → clock →
panel (full re-init + clear, clean base) → BLE/social as they were, and
re-joins the previous WiFi network **asynchronously in the background** (no
20 s DHCP block on wake; gives up quietly after 30 s).

---

## Acceptance Criteria

- [x] **AC1:** Entering the off/sleep state visibly changes the display (sleep still).
- [x] **AC2:** The off indication cannot be mistaken for a frozen normal screen
      (title + dedicated artwork).
- [x] **AC3:** Power path defined: pseudo-off in firmware; the physical switch
      remains a hard cut (see Issue #4).
- [x] **AC4:** True power-cut blanking — N/A for the switch path today (hard cut,
      no warning); revisit with the Issue #4 hardware.
- [x] **AC5:** Waking (CENTER) returns cleanly to normal operation, radios and
      WiFi restored.
- [ ] **AC6:** Verified on hardware — sleep entry (both triggers), wake, battery
      draw while asleep, e-ink ghosting after wake.

---

## Remaining work / future ideas

- [ ] Hardware verification pass (AC6), including measured sleep-state current.
- [ ] SC7A20 INT1 (GP15) motion-wake + true MCU dormant sleep — needs a rework of
      the task split (display task and cyw43 arch don't survive dormant).
- [ ] Parking core 1 while asleep (FreeRTOS SMP has no core-offline API today).
- [ ] Optional: "Power Off" menu item as an explicit alternative to the tap gesture.

---

## Notes / References
- Implementation: `dev-setup/wetgreg-hub-rtos/main.c` — `STATE_SLEEP`,
  `render_sleep_screen()`, `power_sleep_enter()/power_sleep_exit()`,
  `wifi_rejoin_start()/wifi_rejoin_poll()`; panel deep-sleep plumbing in
  `rtos_tasks.c` (`rtos_display_cmd`, `DISP_CMD_SLEEP/WAKE`).
- GitHub Issue #3; strongly related: [Better Power Switch](Feature_PowerSwitchExtender_Status-Incomplete.md) (Issue #4).
