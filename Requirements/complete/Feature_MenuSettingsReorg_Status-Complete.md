# Feature: Main Menu Reorganization — Settings Submenu & Factory Reset

**Status:** ✅ Complete
**Branch:** `feature/menu-settings-reorg`
**Source:** UX housekeeping (2026-07-09) — the flat 10-item main menu had outgrown itself; new features (animations, factory reset) needed a sane home
**Owner:** rompasaurus
**Priority:** Medium
**Type:** UX / menu structure

---

## User Story

> **As a** user navigating the main menu,
> **I want** everyday items up front and configuration tucked into a Settings submenu,
> **so that** the menu is short, scannable, and rarely-used device setup doesn't crowd out daily actions.

---

## What was built

The flat 10-item menu became a 2-level structure:

**Main menu (8 items):** MOOD SELECT · ANIMATIONS · SOUND · MOTION · DEVICE INFO · SOCIAL · SETTINGS · BACK

**Settings submenu (`STATE_SETTINGS`):** NETWORK · BLUETOOTH · DISPLAY · SET TIME · RESET WETGREG · BACK
— the four config screens moved out of the main menu; their LEFT/back now returns to Settings, not the main menu.

**Factory reset (`STATE_RESET_CONFIRM`):** a confirmation card spelling out exactly what is erased (saved WiFi, name + friends met, display + social settings) before re-seeding the saved-store defaults, persisting, and rebooting — the next boot generates a fresh WetGreg identity and plays the splash for the "new" Greg.

The ANIMATIONS entry fronts the Animations submenu (lands with the
[Greg Intro / Tutorial](Feature_GregIntroTutorial_Status-Complete.md) feature).

---

## Acceptance Criteria

- [x] **AC1:** Main menu shows daily-use items only; all device configuration lives under SETTINGS.
- [x] **AC2:** Every screen reached through Settings returns to Settings on LEFT/back (Network menu, Bluetooth, Display, Set Time).
- [x] **AC3:** Factory reset requires an explicit confirmation card listing what will be erased.
- [x] **AC4:** Reset re-seeds defaults, persists to flash, and reboots cleanly (fresh identity + boot splash).
- [ ] **AC5:** Verified on hardware.

---

## Notes / References
- Implementation: `dev-setup/wetgreg-hub-rtos/main.c` — `menu_items[]`/`MENU_IDX_*`,
  `render_settings_menu()`, `render_reset_confirm()`, `STATE_SETTINGS`,
  `STATE_RESET_CONFIRM` cases; reset path reuses `saved_seed_defaults()` + `watchdog_reboot()`.
- Related: [Menu Clarity](Feature_MenuClarity_Status-Incomplete.md) (Issue #5 — directional hints; still open, this reorg does not close it).
