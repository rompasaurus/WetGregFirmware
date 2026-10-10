# Feature: Accessible Power Switch (Extender)

**Status:** ❌ Incomplete
**Branch:** `feature/power-switch-extender`
**Source:** GitHub Issue [#4](https://github.com/rompasaurus/WetGregFirmware/issues/4) — _"Better power switch"_ (reporter: daniel-warner-eucom)
**Owner:** _unassigned_
**Priority:** Medium
**Type:** Hardware / mechanical (⚠️ not firmware — belongs to the enclosure / PCB design)

---

## User Story

> **As a** user who wants to power the unit on or off,
> **I want** to operate the power switch without removing the back cover,
> **so that** I can turn it on/off easily with normal-sized fingers ("otherwise… shenanigans will ensue").

**Reported behavior:** "The power switch requires removing the back cover to operate. Perhaps a 3D-printed extender could be attached to it to allow big fat fingers to move it on & off."

---

## Acceptance Criteria

- [ ] **AC1:** The power switch can be toggled on/off **without opening the enclosure**.
- [ ] **AC2:** The switch is operable with an adult fingertip (no tool required).
- [ ] **AC3:** The solution does not compromise the enclosure's fit, structural integrity, or (if applicable) any water/splash resistance implied by the "Wet Greg" theme.
- [ ] **AC4:** The actuator has clear on/off travel and does not snag or fall off in normal handling.
- [ ] **AC5:** (If a 3D-printed extender) An STL/model is added to the hardware repo and documented.

---

## Implementation Plan

> **Scope note:** This is a mechanical/enclosure change, likely living in the **Dilder-PCB** working directory (`/home/rompasaurus/COdingProjects/Dilder-PCB`) and/or the enclosure CAD, **not** in this firmware repo. Tracked here for completeness so the issue isn't lost.

**Option A — 3D-printed switch extender (reporter's suggestion, lowest effort):**
1. Measure the existing slide/toggle switch actuator and its position relative to the back cover.
2. Add an access slot/hole in the back cover aligned to the switch throw.
3. Design a small printed cap/lever that clips onto the switch nub and protrudes through the slot for finger access.
4. Print, test travel and retention, iterate tolerances.

**Option B — Relocate / change the switch (higher effort):**
1. Move the switch to an edge-accessible position, or swap to a larger externally-mounted rocker/slide, in the next PCB + enclosure revision.

**Option C — Firmware-assisted soft power (complementary):**
1. Add a "Power Off" menu action so the physical switch is needed less often (ties into Issue #3 off-state). The hardware master switch still exists for true power-down / storage.

**Recommendation:** Option A now (quick win), consider Option B for the next board revision, and Option C as a firmware complement.

**Open questions**
- What switch type/footprint is currently used? (Check `Dilder-PCB` BOM / footprint.)
- Is there an enclosure CAD source to modify, or only a printed shell?

---

## Definition of Done / Status

**Current status:** Not started. Hardware task — needs enclosure/PCB owner.

- [ ] Switch type & location documented
- [ ] Extender (or relocation) designed
- [ ] Printed/fabricated and fit-tested on the real enclosure
- [ ] Model files committed to the hardware repo
- [ ] GitHub issue #4 closed → mark **Complete**

---

## Notes / References
- GitHub Issue #4
- Related: [Off-State Screen](Feature_OffStateScreen_Status-Incomplete.md) (Issue #3), enclosure/PCB in `Dilder-PCB`
