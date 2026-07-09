# Feature: Main Menu Directional Clarity

**Status:** ❌ Incomplete
**Branch:** `feature/menu-clarity`
**Source:** GitHub Issue [#5](https://github.com/rompasaurus/WetGregFirmware/issues/5) — _"Menu clarity"_ (reporter: daniel-warner-eucom)
**Owner:** _unassigned_
**Priority:** Medium (discoverability)
**Type:** UX improvement

---

## User Story

> **As a** user on the main menu,
> **I want** the screen to show — in a single word each — what pressing left / right / up / down on the joystick does,
> **so that** I know what to expect from each action before I press it ("otherwise… shenanigans will ensue :D").

---

## Acceptance Criteria

- [ ] **AC1:** The main menu visibly labels each joystick direction (up, down, left, right) with a **single word** describing its action.
- [ ] **AC2:** Labels reflect the actual behavior (e.g. Up/Down = navigate/scroll, Center = select, Left = back) and stay in sync if bindings change.
- [ ] **AC3:** Hints are legible in all valid orientations (landscape-R, landscape-L, portrait) without overlapping menu content.
- [ ] **AC4:** The convention is reusable so other screens can display consistent directional hints (see related Issues #1, #2).
- [ ] **AC5:** Verified on hardware for readability at normal viewing distance.

---

## Implementation Plan

**Relevant code:** `main.c`, `case STATE_MENU:` (~L4165) and its renderer. The menu currently uses Up/Down to move the selection, CENTER/RIGHT-style to enter, and LEFT to go back to the octopus (~L4199–4236).

1. **Confirm the actual binding map** for the main menu:
   - UP → previous item, DOWN → next item, CENTER → select/enter, LEFT → back (to octopus). (Verify against current handler.)
2. **Add a directional hint HUD** to the menu renderer — a compact 4-way legend (or edge labels) showing one word per direction, e.g. `▲ Up  ▼ Down  ● Select  ◀ Back`. Keep each to a single word per the reporter's request.
3. **Orientation-aware placement:** render the legend using the same rotation model as the rest of the UI so it isn't clipped/mirrored in portrait vs landscape.
4. **Centralize the labels** in a small table/helper so the same component can be dropped onto other screens (Motion, Set Time, Sounds) — directly supports Issues #1 and #2 where hint/behavior mismatches caused confusion.
5. **Keep it lightweight** — small font (`lib/Fonts`), no extra frame churn (respect panel refresh cadence).

**Open questions**
- Single shared legend on every screen, or per-screen custom labels? (Recommend a shared helper with per-screen overrides.)
- Exact wording for each direction (e.g. "Back" vs "Exit", "Select" vs "OK").

---

## Definition of Done / Status

**Current status:** Not started.

- [ ] Binding map confirmed for main menu
- [ ] Single-word directional legend rendered on the main menu
- [ ] Legend correct in all 3 orientations
- [ ] Reusable hint helper extracted (for cross-screen consistency)
- [ ] Verified on hardware
- [ ] Merged & GitHub issue #5 closed → mark **Complete**

---

## Notes / References
- GitHub Issue #5
- Related: [Set Time/Date Controls](Feature_SetTimeDateControls_Status-Incomplete.md) (#2), [Motion Screen Exit](Feature_MotionScreenExit_Status-Incomplete.md) (#1) — shared control-hint convention
