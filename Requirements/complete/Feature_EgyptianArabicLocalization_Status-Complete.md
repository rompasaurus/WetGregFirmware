# Feature: Egyptian Arabic Localization — Language Setting + Masri Quote Pack

**Status:** ✅ Complete (hardware verification pending)
**Branch:** `feature/egyptian-arabic`
**Source:** Owner request (2026-10-12) — "an Egyptian Arabic localization… default English, retain across reboots, translate the quotes"
**Owner:** rompasaurus
**Priority:** Medium (audience expansion)
**Type:** Content + rendering + settings

---

## User Story

> **As an** Egyptian WetGreg owner,
> **I want** Greg to talk to me in Egyptian Arabic (masri), not Modern Standard or another dialect,
> **so that** his jokes land the way they do for English speakers.

---

## What was built

- **Settings → LANGUAGE** row (`SET_ITEM_LANGUAGE`): CENTER toggles
  `ENGLISH ↔ ARABIC`. Stored in `g_saved.language` (appended to the flash
  settings image; old images read `0xFF` and sanitise to English, so no magic
  bump). Default is English; factory reset returns to English.
- **Arabic quote pack** `quotes_ar.h`: a 1:1 twin of `quotes.h` — same index,
  same mood, 956 quotes in Cairo-dialect Egyptian Arabic. Puns and idioms were
  adapted rather than translated (كشري, طعمية, النتيجة, روشنة…), the all-ages
  rule applies unchanged, and religion/politics references were avoided. Max
  length 66 characters (hard cap 80).
- **Arabic text renderer** `src/ui/text_ar.c`: UTF-8 decode → contextual
  shaping (isolated/initial/medial/final + lam-alef ligatures) → word-wrapped
  right-to-left layout with digit runs kept left-to-right. ASCII punctuation
  falls back to the 5×7 Latin font on the Arabic baseline.
- **Bitmap fonts** `src/ui/font_ar.h`: Noto Kufi Arabic rasterised to 1-bit by
  `dev-setup/tools/arfont.py` in three sizes — 10 px for menus/labels, 11 px
  for the quote bubble, 16 px for titles and the intro story (presentation
  forms U+FE80–FEFC, Arabic-Indic digits, ، ؟). ~30 KB flash in total.
- **Localized home screen** when Arabic is active: quote bubble (wide: 4 lines;
  tall: bubble grows upward to fit 5 lines), mood tag (`mood_names_ar[]`),
  date/time header with Arabic month names and Arabic-Indic digits (ص/م for
  AM/PM), and the tall layout's steps row ("خطوات").
- **Localized menus and screens** (`src/ui/i18n.c`): every `draw_text()` /
  `draw_inverted_line()` / `draw_text_big()` call goes through an Arabic hook
  in `text_ar.c`. In Arabic mode the English literal is looked up in the string
  table (exact, or "KEY: VALUE" piecewise so live data like IPs, dBm and step
  counts stay as-is), the layout is mirrored (right-aligned, cursor chevron on
  the right, highlight bar sized for the Arabic cell) and the 10 px Kufi UI font
  is used. Main menu, mood picker, settings, display, animations, sound, motion,
  device info, set-time, factory reset, network/WiFi/scan/saved, Bluetooth,
  social, emote and sleep/low-battery screens are covered. The WiFi password
  keyboard, SSIDs, WetGreg auto-names and the I2C debug screen stay Latin.
- **Startup and intro:** the boot splash title becomes "ويت جريج" in the 16 px
  font with the same slide/drop motion; the "who is Greg" story plays its six
  beats in Egyptian Arabic with the same per-letter typewriter reveal; the
  sleep still reads "جريج نايم / شششش....".
- The serial log stays in English.

---

## Acceptance Criteria

- [x] **AC1:** Language selectable from Settings; default English.
- [x] **AC2:** Choice persists across reboot and power-off (flash settings store).
- [x] **AC3:** Every English quote has an Egyptian-Arabic counterpart with the same mood id (validated by script: count, charset, length).
- [x] **AC4:** Arabic renders joined and right-to-left on both orientations within the existing bubble geometry (host-rendered check of `text_ar.c`).
- [ ] **AC5:** Verified on hardware (legibility of 11 px Kufi on the panel; bubble fit for the longest quotes).

---

## Notes for maintainers

- Regenerate the fonts after changing size/face:
  `python3 dev-setup/tools/arfont.py header dev-setup/wetgreg-hub-rtos/src/ui/font_ar.h /usr/share/fonts/noto/NotoKufiArabic-Regular.ttf font_ar_ui:10 font_ar_q:11 font_ar_big:16`
  (`preview <ttf> <size> out.png` renders sample lines first). Needs Pillow + the Noto fonts on the host.
- Adding a new screen string: write the English literal as usual and add a row
  to `src/ui/i18n.c`. Prefer "KEY: VALUE" for lines with live data so only the
  key needs an entry. Strings with no entry simply stay English.
- When quotes are added to `quotes.h`, add the Arabic twin at the same index in
  `quotes_ar.h`; both tables are `QUOTE_COUNT` long so a mismatch fails the build.
- Allowed Arabic charset: letters U+0621–064A (no tashkeel, no tatweel),
  digits ٠–٩, space, `. ، ؟ ! - %`. Anything else draws as a blank.
