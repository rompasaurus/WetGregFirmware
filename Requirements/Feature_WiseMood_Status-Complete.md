# Feature: WISE Mood — Spectacled Greg & Silly-Fact Quote Pack

**Status:** ✅ Complete
**Branch:** `feature/wise-mood`
**Source:** Content expansion (2026-07-09) — a 17th mood delivering neat facts with unearned authority
**Owner:** rompasaurus
**Priority:** Low (content / personality)
**Type:** Content + character art

---

## User Story

> **As a** WetGreg owner,
> **I want** Greg to occasionally lecture me with silly-but-true facts while looking like a distracted professor,
> **so that** the quote rotation stays fresh and the device keeps surprising me.

---

## What was built

- **`MOOD_WISE` (16) / `EXPR_WISE` (18)** — a full 17th mood wired through the
  entire mood pipeline: `mood_names[]`, mood picker, expression cycle
  (`cycle_wise`), and body transform (slow professorial nod + tiny mid-lecture sway).
- **Art:** round white spectacle rims with bridge and temple arms
  (`draw_glasses_wise`), slightly crossed uneven pupils ("big brain, zero
  focus"), and a goofy open tooth grin (`draw_mouth_wise`).
- **Quote pack:** 42 new WISE quotes in `quotes.h` (823 → 865) — real-ish
  facts delivered smugly ("SHARKS EXISTED BEFORE TREES. RESPECT YOUR ELDERS.",
  "WOMBATS POOP CUBES. NATURE HAS A 3D PRINTER.").

---

## Acceptance Criteria

- [x] **AC1:** WISE selectable in the mood picker and included in the ALL rotation.
- [x] **AC2:** Distinct visual identity (glasses + crossed pupils + tooth grin) readable on the 1-bit e-ink at both orientations.
- [x] **AC3:** Dedicated quote pool tagged mood 16; quote count constant updated.
- [x] **AC4:** Expression cycle animates (WISE ↔ OPEN ↔ SMILE) like other moods.
- [ ] **AC5:** Verified on hardware.

---

## Notes / References
- Implementation: `dev-setup/wetgreg-hub-rtos/main.c` — `draw_pupils_wise()`,
  `draw_glasses_wise()`, `draw_mouth_wise()`, `cycle_wise`, `setup_body_transform`
  WISE case; `dev-setup/wetgreg-hub-rtos/quotes.h` — mood-16 pack.
