/**
 * icons.h — small status-bar glyphs (battery / wifi / bluetooth / social).
 *
 * Purpose: provide the small status-bar glyphs shared by the home-screen
 * layouts.
 */
#ifndef ICONS_H
#define ICONS_H

#include <stdbool.h>

void draw_battery_icon(int x0, int y0);   /* reads the filtered g_batt_pct */
void draw_wifi_icon(int x0, int y0, bool connected);
void draw_bt_icon(int x0, int y0);        /* drawn only when paired */
void draw_social_icon(int x0, int y0);    /* shown while proximity scanning */

#endif /* ICONS_H */
