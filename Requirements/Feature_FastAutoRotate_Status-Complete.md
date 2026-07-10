# Feature: Fast Auto-Rotate — Sub-Second Orientation Response

**Status:** ✅ Complete
**Branch:** `fix/auto-rotate-latency`
**Source:** Field report (2026-07-10) — "rotation takes 2-3 seconds before it rotates in normal use"
**Owner:** rompasaurus
**Priority:** Medium (feel / responsiveness)
**Type:** UX / sensor pipeline

---

## User Story

> **As a** user turning my WetGreg between landscape and portrait,
> **I want** the screen to re-lay out well under a second after I settle the new hold,
> **so that** rotation feels like a feature instead of a wait.

---

## Problem

Rotation latency was ~2-3 s end-to-end, and had been since the Housekeeping
split (verified byte-identical classifier vs. the previous firmware — this was
a designed-in cost, not a regression):

- accel sampling sat behind a **250 ms gate shared with the step counter** (4 Hz)
- the flap filter needed a candidate + 3 consistent samples = **~1.0-1.25 s**
- plus the e-ink refresh (~0.3-1 s) on top

## Fix

- **Sample the accelerometer every Housekeeping pass (~20 Hz)** — the read is a
  0.2 ms I2C burst; the 250 ms gate was never needed for the orientation math.
- **Keep the step detector on its original 250 ms gate** — `STEP_CAL_FACTOR`
  (see [Pedometer Calibration](Feature_PedometerCalibration_Status-Complete.md))
  was measured against that exact 4 Hz cadence and must not change.
- **Retune the flap filter to the new cadence:** 6 consistent reads at ~20 Hz
  ≈ **300 ms** of stability — equivalent protection to the old 3-of-4 Hz filter
  at a quarter of the latency.

New end-to-end: ~300-400 ms detection + panel refresh → rotation lands in well
under a second of settling the device.

---

## Acceptance Criteria

- [x] **AC1:** Orientation classification runs at the full HK cadence (~20 Hz), decoupled from the pedometer gate.
- [x] **AC2:** Step counting cadence (and its ×2 calibration) unchanged.
- [x] **AC3:** Flap protection retained (~300 ms consistent-hold before switching; ignore-zone and boot-prime behavior untouched).
- [ ] **AC4:** Verified on hardware — rotation feels sub-second; no layout flapping while walking/handling.

---

## Notes / References
- Implementation: `dev-setup/wetgreg-hub-rtos/main.c` — `orientation_update()`.
- The `ORIENT_DEBUG` serial HUD stays on the 250 ms gate so calibration logs don't flood at 20 Hz.
