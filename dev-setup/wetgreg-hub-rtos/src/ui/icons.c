/**
 * icons.c — status-bar glyphs, drawn in raw canvas coordinates.
 *
 * Purpose: implement the battery/wifi/bluetooth/social glyph drawing in raw
 * canvas coordinates.
 */
#include "icons.h"

#include <stdlib.h>

#include "canvas.h"
#include "power.h"    /* g_batt_pct — filtered by batt_sample, no inline ADC read */

/* ─── Battery icon (16x10 pixels) ─── */
void draw_battery_icon(int x0, int y0) {
    int pct = g_batt_pct;   /* filtered value from batt_sample — no inline ADC read */

    /* Battery outline: 14x8 rectangle + 2x4 terminal nub */
    for (int x = x0; x < x0 + 14; x++) { px_set(x, y0); px_set(x, y0 + 7); }
    for (int y = y0; y < y0 + 8; y++) { px_set(x0, y); px_set(x0 + 13, y); }
    /* Terminal nub on right */
    for (int y = y0 + 2; y < y0 + 6; y++) { px_set(x0 + 14, y); px_set(x0 + 15, y); }

    if (pct < 0) {
        /* USB powered — lightning bolt inside battery */
        px_set(x0 + 8, y0 + 1); px_set(x0 + 7, y0 + 2);
        px_set(x0 + 6, y0 + 2); px_set(x0 + 6, y0 + 3);
        px_set(x0 + 5, y0 + 3); px_set(x0 + 4, y0 + 3);
        px_set(x0 + 5, y0 + 3); px_set(x0 + 9, y0 + 3);
        px_set(x0 + 8, y0 + 3); px_set(x0 + 7, y0 + 3);
        px_set(x0 + 8, y0 + 4); px_set(x0 + 9, y0 + 4);
        px_set(x0 + 7, y0 + 5); px_set(x0 + 8, y0 + 5);
        px_set(x0 + 6, y0 + 6); px_set(x0 + 5, y0 + 6);
    } else {
        /* Fill bars based on percentage (4 bars max) */
        int bars = (pct + 12) / 25;  /* 0-4 bars */
        for (int b = 0; b < bars && b < 4; b++) {
            int bx = x0 + 2 + b * 3;
            for (int y = y0 + 2; y < y0 + 6; y++)
                for (int x = bx; x < bx + 2; x++)
                    px_set(x, y);
        }
    }
}

/* ─── WiFi icon (16x12 pixels) ─── */
void draw_wifi_icon(int x0, int y0, bool connected) {
    for (int i = -6; i <= 6; i++) {
        int ay = y0 + 1;
        if (i >= -5 && i <= 5) ay = y0;
        if (i >= -3 && i <= 3) ay = y0 - 1;
        px_set(x0 + 8 + i, ay);
    }
    for (int i = -4; i <= 4; i++) {
        int ay = y0 + 4;
        if (i >= -3 && i <= 3) ay = y0 + 3;
        if (i >= -1 && i <= 1) ay = y0 + 2;
        px_set(x0 + 8 + i, ay);
    }
    for (int i = -2; i <= 2; i++) {
        int ay = y0 + 6;
        if (i >= -1 && i <= 1) ay = y0 + 5;
        px_set(x0 + 8 + i, ay);
    }
    px_set(x0 + 7, y0 + 8); px_set(x0 + 8, y0 + 8);
    px_set(x0 + 9, y0 + 8); px_set(x0 + 8, y0 + 9);
    if (!connected) {
        for (int i = 0; i < 11; i++) {
            px_set(x0 + 2 + i, y0 + i);
            px_set(x0 + 3 + i, y0 + i);
        }
    }
}

/* Short Bresenham line in canvas space (for the BT glyph). */
static void icon_line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px_set(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Bluetooth rune (~7px wide, 9px tall), stem at x0+3. Drawn only when paired. */
void draw_bt_icon(int x0, int y0) {
    int cx = x0 + 3, w = 3;
    int top = y0, bot = y0 + 8, u = y0 + 2, l = y0 + 6;
    icon_line(cx, top, cx, bot);     /* vertical stem            */
    icon_line(cx, top, cx + w, u);   /* top apex  → upper knee   */
    icon_line(cx + w, u, cx - w, l); /* upper knee → lower-left  */
    icon_line(cx, bot, cx + w, l);   /* bottom apex → lower knee */
    icon_line(cx + w, l, cx - w, u); /* lower knee → upper-left  */
}

/* Social/scanning icon — concentric "broadcast" rings (distinct from the wifi
 * corner-arcs). Shown when proximity scanning is live. ~9px box. */
void draw_social_icon(int x0, int y0) {
    int cx = x0 + 4, cy = y0 + 4;
    px_set(cx, cy);                                                   /* center dot */
    px_set(cx, cy-2); px_set(cx, cy+2); px_set(cx-2, cy); px_set(cx+2, cy);      /* inner ring */
    px_set(cx-1, cy-1); px_set(cx+1, cy-1); px_set(cx-1, cy+1); px_set(cx+1, cy+1);
    px_set(cx, cy-4); px_set(cx, cy+4); px_set(cx-4, cy); px_set(cx+4, cy);      /* outer ring */
    px_set(cx-3, cy-2); px_set(cx+3, cy-2); px_set(cx-3, cy+2); px_set(cx+3, cy+2);
    px_set(cx-2, cy-3); px_set(cx+2, cy-3); px_set(cx-2, cy+3); px_set(cx+2, cy+3);
}
