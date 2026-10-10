# Feature: Old Greg Introduction Video / Animation / Tutorial

**Status:** ✅ Complete
**Branch:** `feature/greg-intro-tutorial`
**Source:** `Notes/notes-summary.md` (Note 2), captured 2026-07-09
**Owner:** rompasaurus
**Priority:** Medium (onboarding / narrative)

---

## User Story

> **As a** first-time WetGreg owner,
> **I want** a short narrated introduction animation that tells me who Greg is,
> **so that** I understand the character and the device's personality before I start using it.

---

## Acceptance Criteria

- [x] **AC1 — Entrance:** Greg drifts in from the **bottom** (portrait) / **left** (landscape), with rising bubbles (shared with the splash).
- [x] **AC2 — Mood portrayal:** sad body/brows/mouth (`MOOD_SAD`/`EXPR_SAD`) plus a nervous scripted gaze — not a static face. His tentacles paddle with a traveling swim-wave (`g_swim_amp` in `row_wobble`).
- [x] **AC3 — Eye/look cycle:** gaze script cycles **up-right → up-left → down-right → down-left** via the new `g_gaze_override` hook in `draw_octopus()`, then repeats at a slower uneasy cadence.
- [x] **AC4 — Idle swim:** after the story Greg keeps swimming in the **left half** (landscape) / **bottom half** (portrait) until the ~38 s hold ends.
- [x] **AC5 — Narrative text:** one story line at a time in big type (`draw_text_big`, word-wrapped 2x font), typewriter-revealed at 60 ms/char, each line owning a 5 s window.
- [x] **AC6 — Story copy:** "Deep in the Atlantic Ocean..." / "Swims a lone octopus..." / "His name is Greg..." / "He is alone..." / "He is far from home." / "And he is WET..."
- [x] **AC7 — Orientation correctness:** side-by-side in landscape, stacked in portrait; orientation re-read every frame like the splash.
- [x] **AC8 — Non-blocking scheduler:** frame pacing sleeps on the input queue (`INTRO_FRAME_MS` = 450 ms, above the panel drain) — Input/Display tasks never starve.
- [x] **AC9 — Entry point:** deliberate trigger via **Menu → ANIMATIONS → INTRO** (`STATE_ANIM_MENU`), a submenu with room for future animations. Not forced on power-on.
- [x] **AC10 — Exit / skip:** any press exits to `STATE_OCTOPUS` with a chirp; the intro re-arms so it can be replayed from the menu.

---

## Definition of Done / Status

- [x] Trigger policy decided: menu-only (Animations submenu); no first-boot autoplay
- [x] Gaze + sad/scared expression implemented in `draw_octopus()` (`g_gaze_override`)
- [x] Text reveal implemented and paced for readability (one line at a time, big type)
- [ ] Verified on hardware in landscape + portrait
- [x] Skippable exit confirmed (any key)
- [x] Merged → marked **Complete** and renamed to `..._Status-Complete.md`

---

## Notes / References
- Implementation: `dev-setup/wetgreg-hub-rtos/main.c` — `STATE_INTRO` + `STATE_ANIM_MENU`,
  `render_intro()`, `intro_draw_story()`, `draw_text_big()`, `draw_pupils_gaze()`,
  swim-wave in `row_wobble()`.
- Source note images: `Notes/IMG20260709142454.jpg`, `Notes/IMG20260709142700.jpg`
- Summary: `Notes/notes-summary.md`
- Related feature: [Splash / Boot Animation](Feature_SplashBootAnimation_Status-Complete.md) (shares bubbles + ease helpers)
