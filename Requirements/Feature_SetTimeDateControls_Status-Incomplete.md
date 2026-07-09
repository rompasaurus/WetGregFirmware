# Feature: Intuitive Set Time / Date (and Similar) Controls

**Status:** ❌ Incomplete
**Branch:** `feature/settime-controls`
**Source:** GitHub Issue [#2](https://github.com/rompasaurus/WetGregFirmware/issues/2) — _"Set Time and Date controls feel backwards"_ (reporter: tomddean)
**Owner:** _unassigned_
**Priority:** Medium (UX)
**Type:** UX improvement

---

## User Story

> **As a** user setting the date and time,
> **I want** the joystick directions to match the screen layout — up/down to move between the stacked fields, left/right to change a value,
> **so that** editing feels natural instead of backwards.

**Reported behavior:** "The time and date setting is done through a vertical stack of editable fields, yet traversing from field to field is done using a L/R press. Suggest changing fields using U/D, and then use L/R to change the values instead of the other way around. I believe some other pages have a similar weird feel (Sounds, I think?)."

---

## Acceptance Criteria

- [ ] **AC1 — Field navigation:** **UP/DOWN** moves the selection between the vertically stacked fields (year, month, day, hour, minute).
- [ ] **AC2 — Value change:** **LEFT/RIGHT** decrements/increments the currently selected field's value directly (modeless — no separate "edit mode" toggle required).
- [ ] **AC3 — Commit/back:** A clear, discoverable action commits the time and returns to the menu (e.g. CENTER = save, or a dedicated "Done"), documented by the on-screen control hints.
- [ ] **AC4 — Wrap-around:** Values wrap correctly (month 12→1, hour 23→0, etc.) as they do today.
- [ ] **AC5 — Hints match behavior:** On-screen control hints reflect the new U/D = field, L/R = value scheme.
- [ ] **AC6 — Consistency audit:** Other stacked-field / adjustment screens (e.g. **Sounds**, and any similarly modal screen) are reviewed and made consistent with this convention.

---

## Implementation Plan

**Relevant code:** `main.c`, `case STATE_SET_TIME:` (~L4279).

**Current design (modal):** two modes gated by `settime_editing`.
- NAV mode: U/D pick a field, CENTER enters edit, LEFT commits + back.
- EDIT mode: U/D change the value, CENTER/LEFT confirm back to NAV.

The reporter wants a **modeless** scheme: U/D always selects the field, L/R always changes the value.

1. **Remove the `settime_editing` mode split** (or repurpose it): map
   - `INPUT_UP`/`INPUT_DOWN` → `settime_field` prev/next (already the NAV behavior).
   - `INPUT_LEFT`/`INPUT_RIGHT` → decrement/increment `settime_dt` for the selected `settime_field` (reuse the ±delta switch currently under EDIT mode).
   - Choose a commit action: `INPUT_CENTER` → save (`rtc_set_datetime`, `ntp_synced = false`) and return to `STATE_MENU`. (Frees LEFT for value editing.)
2. **Update `render_set_time()`** to highlight the selected field and show hints: "U/D: field  L/R: change  ●: save".
3. **Confirm exit path** — ensure there is always an obvious way back (CENTER save, or add explicit back). Verify no "trapped" feeling (cross-check with Issue #1 philosophy).
4. **Audit `STATE_SOUND`** (~L4364) and other adjustment screens; apply the same U/D-navigate / L/R-adjust convention where it makes sense. Track any changed screens in this doc.
5. **Optional:** extract a shared "stacked field editor" helper so all such screens behave identically.

**Open questions**
- Preferred commit control — CENTER, or a dedicated "Done" field at the bottom of the stack?
- Confirm exactly which other screens share the "backwards" feel (reporter flagged Sounds as a guess).

---

## Definition of Done / Status

**Current status:** Not started. Current implementation is modal (CENTER toggles edit mode).

- [ ] SET_TIME converted to modeless U/D-field, L/R-value
- [ ] Control hints updated
- [ ] Sounds (and other flagged screens) audited & aligned
- [ ] Verified on hardware
- [ ] Merged & GitHub issue #2 closed → mark **Complete**

---

## Notes / References
- GitHub Issue #2
- Related: [Menu Clarity](Feature_MenuClarity_Status-Incomplete.md) (control-hint conventions), [Motion Screen Exit](Feature_MotionScreenExit_Status-Incomplete.md)
