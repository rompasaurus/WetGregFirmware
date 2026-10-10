# Feature: Wet Greg Opening Splash / Boot Animation

**Status:** ✅ Complete
**Branch:** `feature/splash-boot-animation`
**Source:** `Notes/notes-summary.md` (Note 1), captured 2026-07-09
**Owner:** _unassigned_
**Priority:** Medium (polish / first-impression)

---

## User Story

> **As a** WetGreg device owner powering on my unit,
> **I want** a short, characterful splash animation of Greg sliding into place with his title,
> **so that** the device feels alive and branded from the very first moment, before it settles into its normal idle screen.

---

## Acceptance Criteria

- [x] **AC1 — Trigger:** The splash animation plays once, automatically, on power-on / boot, before the normal octopus + quotes idle screen (`STATE_OCTOPUS`) is shown.
- [x] **AC2 — Landscape entrance:** In a landscape hold (`OR_LAND_R` / `OR_LAND_L`), Greg peeks in staring from the **left side** and, over ~2 seconds, moves into the default speech position.
- [x] **AC3 — Portrait entrance:** In a portrait hold (`OR_TALL`), Greg peeks in from the **bottom** and, over ~2 seconds, slides into the default position.
- [x] **AC4 — Bubbles:** Water bubbles appear around the screen during the entrance in both orientations.
- [x] **AC5 — Title reveal (landscape):** The "Wet Greg" title slides in **from the right** after Greg reaches position.
- [x] **AC6 — Title reveal (portrait):** The "Wet Greg" title slides in **from the top**, accompanied by a bubble.
- [x] **AC7 — Sassy variant:** A "sassy" version of the Greg animation is used while he slides into place.
- [x] **AC8 — Total runtime:** The full animation runs for **≈10 seconds**, then hands off to the existing default quotes screen (`STATE_OCTOPUS`).
- [x] **AC9 — Orientation correctness:** The animation respects the live orientation classifier and uses the correct display rotation per `ORIENT[]` (no upside-down / mirrored art).
- [x] **AC10 — Non-blocking scheduler:** The animation must not starve the FreeRTOS Input/Display tasks or soft-lock the panel (follows existing frame-submit cadence; e-paper/panel refresh timing respected).
- [x] **AC11 — Skippable (recommended):** A joystick press during the splash short-circuits straight to `STATE_OCTOPUS`.

---

## Implementation Plan

**Relevant code:** `main.c` — state machine (`STATE_*` defines ~L222+), `draw_octopus()` (~L1677), `render_octopus_wide()` / `render_octopus_tall()` (~L1740/1772), orientation table `ORIENT[]` (~L187), `transpose_to_display()` / `display_render()`, `POLL_INPUT`/`POLL_END` macros. Boot sequence in `main()` before the `while` loop (~L4034).

1. **Add a new state** `STATE_SPLASH` (new `#define`) and set it as the initial `state` value at boot instead of `STATE_OCTOPUS`.
2. **Define animation timeline** as a small keyframe/phase model driven by `to_ms_since_boot()`:
   - Phase A (0–2 s): Greg enters + eases to default position (orientation-dependent start offset: left-edge X for landscape, bottom-edge Y for portrait).
   - Phase B (2–~4 s): title slides in (right→center for landscape, top→center for portrait) + bubble.
   - Phase C (~4–10 s): hold / idle sassy loop, bubbles drifting.
3. **Reuse `draw_octopus()`** with a positional offset parameter (extend `layout_ox`/`layout_oy` usage already present in tall/wide renderers) to slide Greg in; add a "sassy" expression/frame variant (reuse existing expression/frame indices where possible).
4. **Bubbles:** reuse or extend the existing bubble drawing used in the chat-bubble/quote path; scatter simple filled circles with per-frame drift.
5. **Title asset:** render "Wet Greg" as text (existing font in `lib/Fonts`) or a bitmap in `assets/`; animate its X (landscape) / Y (portrait) with easing.
6. **Handoff:** when elapsed ≥ ~10 s (or a joystick press is read via `POLL_INPUT`), set `state = STATE_OCTOPUS`, reset `frame_idx`, and pick a quote (`pick_quote()`).
7. **Guard timing:** keep frame submission at or above the panel drain cadence (mirror the note in the `STATE_MOTION` handler about not out-running the ~0.7 s refresh) so the Input task is never starved.

**Open questions**
- Is the target panel the e-paper (`lib/e-Paper`) or an LCD? Frame rate for a 10 s slide differs greatly — a slow e-paper refresh may force a "few keyframes" approach rather than smooth motion. _Confirm target display variant (V4)._
- Is there an existing "sassy" art frame, or does new art need to be authored into `assets/`?

---

## Definition of Done / Status

**Current status:** Complete 2026-07-09 — implemented, built clean (UF2 produced), and verified on hardware.

Implementation decisions (see `dev-setup/wetgreg-hub-rtos/main.c`, `STATE_SPLASH`):
- Target panel is the WeAct 2.13" V4 e-ink → "few keyframes" approach: one frame per ~450 ms (`SPLASH_FRAME_MS`), paced exactly like `STATE_EMOTE_PLAY` so the Input task is never starved (AC10).
- "Sassy" variant = existing art reused: side-eye `MOOD_CHILL` pupils + `EXPR_SMIRK` smirk, plus a small bob on the layout origin during the slide (AC7 — no new art needed).
- Title is `draw_text_2x` text ("WET GREG" one line in landscape; "WET"/"GREG" stacked in portrait, since 2x text is wider than the 122 px tall canvas).
- Any joystick press skips (AC11); handoff picks a fresh quote and resets `frame_idx`.
- Boot orientation prime (AC9): the auto-rotate classifier's FIRST post-boot verdict applies immediately (`g_orient_primed`, no hysteresis), and the splash holds its first frame up to ~700 ms for it — otherwise the splash always started landscape and flipped mid-entrance when the device was held portrait. Steady-state rotation still goes through the 3-stable-read hysteresis.
- Stale-hold guard (AC11, `rtos_tasks.c` Input task): a key already down when the Input task starts — i.e. the CENTER hold that just triggered the 10 s watchdog reboot — no longer reads as a press edge (and never auto-repeats), so it can't phantom-skip the splash. Skipping requires a fresh press after boot.

- [x] Design reviewed (art + timeline confirmed for target panel)
- [x] Implemented behind `STATE_SPLASH`
- [x] Verified on hardware in all 3 valid holds (landscape-R, landscape-L, portrait)
- [x] Confirmed ~10 s runtime and clean handoff to quotes screen
- [x] Merged → mark this document **Complete** and rename to `..._Status-Complete.md`

---

## Notes / References
- Source note image: `Notes/IMG20260709141712.jpg`
- Summary: `Notes/notes-summary.md`
- Related feature: [Greg Introduction / Tutorial](Feature_GregIntroTutorial_Status-Incomplete.md)
