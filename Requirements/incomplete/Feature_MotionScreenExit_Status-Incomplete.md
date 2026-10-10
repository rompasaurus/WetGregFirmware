# Feature: Reliable Exit from the "Motion" Screen

**Status:** ❌ Incomplete
**Branch:** `fix/motion-screen-exit`
**Source:** GitHub Issue [#1](https://github.com/rompasaurus/WetGregFirmware/issues/1) — _"no apparent way to exit the 'Motion' screen"_ (reporter: tomddean)
**Owner:** _unassigned_
**Priority:** High (usability bug — user gets stuck)
**Type:** Bug fix

---

## User Story

> **As a** user viewing the Motion (accelerometer / pedometer) screen,
> **I want** the advertised "L" back control to reliably take me out,
> **so that** I never feel trapped on that screen.

**Reported behavior:** "I seem to get stuck in the 'Motion' screen. The controls presented at the bottom say 'L' should go back but it doesn't seem to be registering."

---

## Acceptance Criteria

- [ ] **AC1:** Pressing LEFT on the Motion screen reliably returns to the main menu (`STATE_MENU`) on the first press, every time.
- [ ] **AC2:** The LEFT press registers even while the screen is mid-refresh of its live accel/step readout.
- [ ] **AC3:** No perceptible "dead" window where input is ignored (or any such window is short enough to be unnoticeable to the user).
- [ ] **AC4:** The on-screen control hint ("L" = back) accurately matches actual behavior.
- [ ] **AC5:** Verified on physical hardware, including rapid/held presses.

---

## Implementation Plan

**Relevant code:** `main.c`, `case STATE_MOTION:` (~L4938). A LEFT→`STATE_MENU` transition already exists (~L4958). The in-code comment (~L4944–4948) documents the root cause: at a 500 ms refresh the menu resubmitted frames faster than the panel could drain, so both framebuffers were never free simultaneously and the **Input task could never emit a press** — LEFT could not exit and the device appeared locked. It was mitigated to a 2 s refresh.

**Hypothesis:** the fix is a mitigation, not a cure — under some timing the Input task is still starved by the display-frame backpressure, so occasional presses are dropped.

1. **Reproduce:** confirm whether LEFT drops are still occurring at the 2 s cadence, and correlate with framebuffer availability.
2. **Decouple input from frame backpressure:** ensure the Input task can always latch a joystick edge regardless of Display task state — e.g. queue/flag the press in the ISR/Input task (interrupt path exists but is gated off, see `joystick_irq_cb` ~L110 and `INPUT_ACQUISITION_MODE` in `rtos_tasks.h`) rather than depending on a free framebuffer.
3. **Prefer edge-latched navigation:** treat LEFT as a latched event consumed on the next poll, so a press during refresh is not lost.
4. **Re-evaluate refresh cadence:** keep the live readout fresh without saturating the panel; consider only redrawing when the step/accel value actually changes.
5. **Audit sibling screens** for the same starvation pattern (any screen that resubmits frames on a tight timer, e.g. I2C scan wait, other live readouts).

**Open questions**
- Should the gated interrupt-driven input mode be finished/enabled as the proper fix (see `rtos_tasks.h` mode notes)?

---

## Definition of Done / Status

**Current status:** Partially mitigated in current code (refresh slowed to 2 s); root cause (input starvation under display backpressure) may not be fully resolved.

- [ ] Root cause confirmed
- [ ] LEFT exit reliable under stress test
- [ ] Sibling live-refresh screens audited for the same bug
- [ ] Verified on hardware
- [ ] Merged & GitHub issue #1 closed → mark **Complete**

---

## Notes / References
- GitHub Issue #1
- Related: interrupt vs polling input acquisition notes in `rtos_tasks.h`
