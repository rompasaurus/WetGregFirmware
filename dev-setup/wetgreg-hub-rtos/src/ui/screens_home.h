/**
 * screens_home.h — the main octopus screen: wide (side-by-side) and tall
 * (longways) layouts, the clock header, and the calibration HUD.
 *
 * Purpose: render the main octopus screen in both holds, plus the clock
 * header and the calibration HUD.
 */
#ifndef SCREENS_HOME_H
#define SCREENS_HOME_H

#include <stdint.h>

#include "quotes.h"   /* Quote type only — the array is app_main.c's */

/* Centered "APRIL 12, 2026  3:47 PM" header row (wide canvas, raw px). */
void draw_clock_header(void);

/* WIDE layout: octopus left, chat bubble + quote right (the classic view). */
void render_frame(const Quote *q, int expr, uint32_t frame_idx);

/* TALL "longways" layout (122x250): 2-row status bar / quote / octopus at
 * the bottom / mood + steps block. */
void render_octopus_tall(const Quote *q, int expr, uint32_t frame_idx);

/* On-screen orientation HUD for calibration; no-op unless ORIENT_DEBUG. */
void draw_orient_hud(void);

#endif /* SCREENS_HOME_H */
