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
- **Arabic quote pack** `quotes_ar.h`: a stand-alone set of **945 original
  Egyptian-Arabic one-liners**, written from the ground up for Egyptian users
  (not a translation of `quotes.h`). 55 per mood (60 for HUNGRY and FULL),
  Cairo/Alexandria dialect, built on Egyptian daily life (الميكروباص، الفكة،
  جروب العيلة، البواب، النتيجة), food culture (كشري، فول، ملوخية وشهقتها، محشي،
  كحك), places (الكورنيش، الترماي، التحرير، الغردقة), bent proverbs (ابن الوز
  عوام، على قد لحافك…), light film/TV/cartoon allusions (بكار، سبيس تون، مسرح
  مصر) and Egyptian comedy devices (القفشة، المبالغة، التريقة على النفس، الرد
  الجاهز). The voice, reference bank, mood briefs and hard rules live in
  `docs/12-ARABIC-QUOTE-STYLE-GUIDE.md`. Rules beyond the English all-ages
  rule: no religion, no politics, no substances, no regional stereotypes, no
  death idioms. Max length 74 characters (hard cap 80); every quote verified
  to fit both bubbles.
- The table has its own length and mood tags: `pick_quote()` filters whichever
  table is active, `quote_at()` clamps a stale index, and toggling the language
  re-picks a quote.
- **Arabic text renderer** `src/ui/text_ar.c`: UTF-8 decode → contextual
  shaping (isolated/initial/medial/final + lam-alef ligatures) → word-wrapped
  right-to-left layout with digit runs kept left-to-right. ASCII punctuation
  falls back to the 5×7 Latin font on the Arabic baseline.
- **Bitmap fonts** `src/ui/font_ar.h`: Noto Kufi Arabic rasterised to 1-bit by
  `dev-setup/tools/arfont.py` in three sizes — 11 px for menus/labels, 13 px
  for the quote bubble, 18 px for titles and the intro story (presentation
  forms U+FE80–FEFC, Arabic-Indic digits, ، ؟). ~40 KB flash in total. The
  quote bubbles grow in Arabic mode to hold the bigger glyphs (wide: 74 px
  tall, 4 lines; tall: full width, 5 lines) and every quote is checked to fit.
  In the tall layout the block under Greg (mood name, steps row) uses the
  18 px font, with the octopus moved 4 px up to make room.
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
- [x] **AC3:** Every mood has a full set of original Egyptian-Arabic quotes (55+), validated by script: format, charset, length, duplicates, banned words.
- [x] **AC4:** Arabic renders joined and right-to-left on both orientations within the existing bubble geometry (host-rendered check of `text_ar.c`).
- [ ] **AC5:** Verified on hardware (legibility of 11 px Kufi on the panel; bubble fit for the longest quotes).

---

## Notes for maintainers

- Regenerate the fonts after changing size/face:
  `python3 dev-setup/tools/arfont.py header dev-setup/wetgreg-hub-rtos/src/ui/font_ar.h /usr/share/fonts/noto/NotoKufiArabic-Regular.ttf font_ar_ui:11 font_ar_q:13 font_ar_big:18`
  (`preview <ttf> <size> out.png` renders sample lines first). Needs Pillow + the Noto fonts on the host.
- Adding a new screen string: write the English literal as usual and add a row
  to `src/ui/i18n.c`. Prefer "KEY: VALUE" for lines with live data so only the
  key needs an entry. Strings with no entry simply stay English.
- The two quote tables are independent: add English lines to `quotes.h` and
  Arabic lines to `quotes_ar.h` (bump `QUOTE_AR_COUNT`). Write new Arabic lines
  in the voice of `docs/12-ARABIC-QUOTE-STYLE-GUIDE.md`, never as translations.
- Allowed Arabic charset: letters U+0621–064A (no tashkeel, no tatweel),
  digits ٠–٩, space, `. ، ؟ ! - %`. Anything else draws as a blank.
