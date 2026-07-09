# Feature: Visible "Off" State Indication

**Status:** ❌ Incomplete
**Branch:** `feature/off-state-screen`
**Source:** GitHub Issue [#3](https://github.com/rompasaurus/WetGregFirmware/issues/3) — _"Screen clear to denote off state"_ (reporter: daniel-warner-eucom)
**Owner:** _unassigned_
**Priority:** Medium
**Type:** UX / hardware-adjacent

---

## User Story

> **As a** user turning the unit off,
> **I want** the screen to visibly show that it is off,
> **so that** I'm not confused about whether it's still on ("otherwise… shenanigans will ensue").

---

## Acceptance Criteria

- [ ] **AC1:** When the unit is switched off (or enters its off/sleep state), the display visibly changes to unambiguously indicate "off".
- [ ] **AC2:** The off indication is stable and does not look like a frozen/normal screen (a persistent-but-idle e-paper image must not be mistaken for a running device).
- [ ] **AC3:** Behavior is well-defined for the actual power path — whether "off" is a true hardware power cut or a low-power/sleep state (see Open Questions).
- [ ] **AC4:** If power is truly cut, the last-rendered frame is either cleared/blanked or shows an explicit "Off" / blank screen before power loss.
- [ ] **AC5:** Turning back on returns to normal operation cleanly.
- [ ] **AC6:** Verified on hardware — the off state is obvious from across a room.

---

## Implementation Plan

**Relevant code:** `main.c` — display driver (`lib/e-Paper`, `display_render()`), power/VBUS sensing (~L2094, `batt`/`usb` fields in `rtos_tasks.h`), power switch behavior. Depends on how the hardware power switch is wired (see also Issue #4).

**Key consideration — e-paper vs LCD:** if the panel is e-paper it *retains* the last image with no power, which is exactly the confusion the reporter describes. The fix is to **actively draw a clear/off frame before power is removed** or before entering deep sleep.

1. **Clarify power model:** does the switch cut MCU power entirely, or trigger a firmware-observable shutdown/sleep? Determine whether firmware gets a chance to run before power loss.
2. **If firmware-observable shutdown** (e.g. switch → GPIO, or a "power off" menu action):
   - Render an explicit off screen (full blank / "Off" / sleepy Greg) via the display driver, wait for the panel to finish, then cut power / enter deep sleep.
3. **If hard power cut with no warning:**
   - Consider a hardware/RC hold or supervisor so firmware can blank the e-paper before rails collapse, **or**
   - Add a firmware "Power Off" menu item that blanks the screen and enters the lowest-power state, making the physical switch secondary.
4. **Design the off frame:** simplest = full white/black clear; nicer = a small "Greg is sleeping / Off" graphic (reuse octopus art with eyes closed).
5. **Verify e-paper full-clear** is used (not a partial refresh) so no ghosting remains.

**Open questions**
- Is the display e-paper (retains image) or LCD (goes dark on its own)? This determines whether any firmware work is needed at all.
- Does the current power switch allow firmware to run a shutdown sequence, or is it a hard rail cut? (Tightly coupled to Issue #4 — Better power switch.)

---

## Definition of Done / Status

**Current status:** Not started. Requires clarification of the hardware power path first.

- [ ] Power model clarified (hard cut vs firmware-observable)
- [ ] Off-frame designed and rendered before power-down / sleep
- [ ] e-paper fully cleared (no ghosting) if applicable
- [ ] Verified on hardware
- [ ] Merged & GitHub issue #3 closed → mark **Complete**

---

## Notes / References
- GitHub Issue #3
- Strongly related: [Better Power Switch](Feature_PowerSwitchExtender_Status-Incomplete.md) (Issue #4) — same power path
