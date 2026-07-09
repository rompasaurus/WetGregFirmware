# Feature: Old Greg Introduction Video / Animation / Tutorial

**Status:** ❌ Incomplete
**Branch:** `feature/greg-intro-tutorial`
**Source:** `Notes/notes-summary.md` (Note 2), captured 2026-07-09
**Owner:** _unassigned_
**Priority:** Medium (onboarding / narrative)

---

## User Story

> **As a** first-time WetGreg owner,
> **I want** a short narrated introduction animation that tells me who Greg is,
> **so that** I understand the character and the device's personality before I start using it.

---

## Acceptance Criteria

- [ ] **AC1 — Entrance:** Greg slides up / in from the **bottom** (portrait) or from the **left** (landscape), with bubbles floating across the screen.
- [ ] **AC2 — Mood portrayal:** Greg appears **sad / scared / frightened / alone**, conveyed through animation and eye orientation (not just a static face).
- [ ] **AC3 — Eye/look cycle:** The animation cycles Greg's gaze: look **up-right → up-left**, then **down-right → down-left** (expressive, slightly nervous motion).
- [ ] **AC4 — Idle swim:** After the intro beat, Greg **remains swimming around** in the **left half** of the screen (landscape) or the **bottom half** (portrait).
- [ ] **AC5 — Narrative text:** Explanatory text **fades/transitions in slowly**, paced so the user can comfortably read it, introducing Wet Greg.
- [ ] **AC6 — Story copy:** The narration reveals, line by line:
  - "Deep in the Atlantic Ocean swims a lone octopus…"
  - "His name is Greg…"
  - "He is alone…"
  - "He is far from home."
  - "And he is WET…"
- [ ] **AC7 — Orientation correctness:** Text region and Greg's swim region adapt to orientation (side-by-side in landscape, stacked in portrait) using the existing rotation model.
- [ ] **AC8 — Non-blocking scheduler:** Playback must not starve the Input/Display tasks or lock the panel; input remains responsive.
- [ ] **AC9 — Entry point:** The intro is reachable deliberately (e.g. a menu item / first-boot flag), **not** forced on every power-on. _Exact trigger TBD — see Open Questions._
- [ ] **AC10 — Exit / skip:** The user can exit the intro at any time (LEFT / any press) and land on the normal octopus screen (`STATE_OCTOPUS`).

---

## Implementation Plan

**Relevant code:** `main.c` — state machine, `draw_octopus()` (~L1677) incl. eyes/pupils/brows/mouth compositing, `render_octopus_wide()`/`render_octopus_tall()`, emote playback path (`STATE_EMOTE_PLAY` ~L242, `render_emote_octopus()` ~L2609) which already animates the octopus over a `tick` in both orientations — a strong base to build on.

1. **Add state** `STATE_INTRO` with an elapsed-time / step driver (`to_ms_since_boot()`), similar in structure to the emote playback renderer.
2. **Gaze control:** `draw_octopus()` already composites pupils/eyes — add a gaze-direction parameter (offset the pupils) and script the up-right → up-left → down-right → down-left sequence over time. Pick a "sad/scared" expression index for the mouth/brows.
3. **Swim motion:** animate `layout_ox`/`layout_oy` in a gentle bob/drift confined to the left half (landscape) or bottom half (portrait), reusing the emote-renderer positioning approach.
4. **Text reveal:** render the 5 story lines sequentially in the opposite half of the screen. Implement a slow reveal (per-line, or typewriter/character-wise, or fade if panel supports it) paced ~1.5–3 s per line so total runtime is comfortable to read.
5. **Bubbles:** reuse the bubble drawing used by the splash feature (shared helper recommended — coordinate with `feature/splash-boot-animation`).
6. **Trigger:** add a menu entry (e.g. under the main `STATE_MENU`) such as "Intro" / "About Greg"; optionally auto-play once on first-ever boot using a persisted flag (there is an existing first-boot persistence pattern near `saved_seed_defaults()` ~L3589 and social-id assignment ~L3601).
7. **Exit:** `POLL_INPUT` loop honours LEFT / any press → `state = STATE_OCTOPUS`.

**Open questions**
- **Trigger policy:** menu-only, first-boot auto-play, or both? (AC9)
- **Text rendering capability:** does the target panel support fade, or only discrete redraws? Determines "slowly transition" implementation.
- Is there sad/scared art already, or is new expression art needed in `assets/emotion-previews`?
- Relationship to the splash feature — should this share a bubble/animation helper module to avoid duplication?

---

## Definition of Done / Status

**Current status:** Not started. Requirement drafted from handwritten notes on 2026-07-09.

- [ ] Trigger policy decided (menu vs first-boot vs both)
- [ ] Gaze + sad/scared expression implemented in `draw_octopus()`
- [ ] Text reveal implemented and paced for readability
- [ ] Verified on hardware in landscape + portrait
- [ ] Skippable exit confirmed
- [ ] Merged → mark **Complete** and rename to `..._Status-Complete.md`

---

## Notes / References
- Source note images: `Notes/IMG20260709142454.jpg`, `Notes/IMG20260709142700.jpg`
- Summary: `Notes/notes-summary.md`
- Related feature: [Splash / Boot Animation](Feature_SplashBootAnimation_Status-Complete.md) (share bubble/animation helpers)
