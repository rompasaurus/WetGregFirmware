# Feature: Pedometer Step-Count Calibration

**Status:** ✅ Complete
**Branch:** `fix/pedometer-calibration`
**Source:** On-body measurement (2026-07-09) — displayed steps landed at roughly half of actual steps walked
**Owner:** rompasaurus
**Priority:** Low (accuracy fix)
**Type:** Sensor calibration

---

## User Story

> **As a** user carrying my WetGreg,
> **I want** the STEPS counter to roughly match the steps I actually take,
> **so that** the pedometer is a fun-but-honest stat instead of a systematic undercount.

---

## Problem & Fix

The step detector counts threshold crossings of the accel magnitude, but it is
sampled at ~4 Hz (behind `orientation_update`'s 250 ms gate). At a normal
~2 step/s walking cadence roughly every other impact falls between samples, so
the raw count measured on-body was about **half** of actual steps.

Fix: each detected crossing now counts as **2 steps** (`STEP_CAL_FACTOR`),
documented at the detector so the constant survives future refactors.

---

## Acceptance Criteria

- [x] **AC1:** Detected crossings are scaled by a named, documented calibration constant (no magic number).
- [x] **AC2:** The rationale (4 Hz sampling vs ~2 step/s cadence) is captured in code comments and this doc.
- [ ] **AC3:** Re-verified on-body over a counted walk (e.g. 200 real steps → 180–220 displayed).

---

## Notes / References
- Implementation: `dev-setup/wetgreg-hub-rtos/main.c` — `STEP_CAL_FACTOR` in `pedometer_update()`.
- Future: raise the detector sample rate or use the SC7A20's hardware features instead of scaling.
