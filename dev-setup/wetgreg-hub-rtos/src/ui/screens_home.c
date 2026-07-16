/**
 * screens_home.c — the main octopus screen (wide + tall layouts).
 *
 * Purpose: compose the wide and tall home layouts from the octopus, icons,
 * text, and live status modules.
 */
#include "screens_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "bt.h"
#include "canvas.h"
#include "icons.h"
#include "motion.h"
#include "octopus.h"
#include "rtc_clock.h"
#include "rtc_compat.h"
#include "text.h"
#include "wifi.h"

/* TEMP debug: draw a marker square at the computed left-eye position in the
 * tall layout, to separate face-position bugs from face-draw bugs. */
#define FACE_DEBUG 0

void draw_clock_header(void) {
    datetime_t t;
    rtc_get_datetime(&t);

    /* Format: "APRIL 12, 2026  3:47 PM" */
    char buf[48];
    int hr12 = t.hour % 12;
    if (hr12 == 0) hr12 = 12;
    const char *ampm = (t.hour < 12) ? "AM" : "PM";
    snprintf(buf, sizeof(buf), "%s %d, %d  %d:%02d %s",
             month_names[t.month - 1], t.day, t.year, hr12, t.min, ampm);

    /* Center the header (6px per char) */
    int len = (int)strlen(buf);
    int header_w = len * 6;
    int header_x = (IMG_W - header_w) / 2;
    if (header_x < 0) header_x = 0;

    /* draw_text uses raw px_set (no offset) — renders at y=1, top of screen */
    draw_text(header_x, 1, buf, IMG_W);
}

/* ─── Chat bubble (wide layout) ─── */
static void draw_bubble(void) {
    int bx = 75, by = 5 + Y_OFF, bw = 170, bh = 70;
    /* Top/bottom edges (double thick) */
    for (int x = bx + 3; x < bx + bw - 3; x++) {
        px_set(x, by); px_set(x, by + 1);
        px_set(x, by + bh - 1); px_set(x, by + bh - 2);
    }
    /* Left/right edges */
    for (int y = by + 3; y < by + bh - 3; y++) {
        px_set(bx, y); px_set(bx + 1, y);
        px_set(bx + bw - 1, y); px_set(bx + bw - 2, y);
    }
    /* Rounded corners */
    int corners[][2] = {{bx+2,by+2},{bx+bw-3,by+2},{bx+2,by+bh-3},{bx+bw-3,by+bh-3}};
    for (int c = 0; c < 4; c++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (abs(dx) + abs(dy) <= 1)
                    px_set(corners[c][0]+dx, corners[c][1]+dy);
    /* Speech tail */
    int tb = 35 + Y_OFF;
    static const int8_t tail_dx[] = {0,-1,-2,-3,-4,-5,-6,-7,-6,-5,-4,-3,-2,-1,0};
    static const int8_t tail_dy[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 7, 6, 5, 4};
    for (int i = 0; i < 15; i++)
        px_set(bx + tail_dx[i], tb + tail_dy[i]);
}

/* ── WIDE layout: octopus left, chat bubble + quote right (the classic view) ── */
void render_frame(const Quote *q, int expr, uint32_t frame_idx) {
    set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    layout_ox = 0; layout_oy = 0;

    draw_clock_header();
    draw_octopus(q, expr, frame_idx);

    /* Chat bubble + quote */
    draw_bubble();
    draw_text(81, 11 + Y_OFF, q->text, 158);

    /* Tagline — current mood/emotion name */
    int tag_y = 5 + 70 + 5 + Y_OFF;
    if (tag_y + 7 < IMG_H) {
        char mood_tag[40];
        snprintf(mood_tag, sizeof(mood_tag), "- %s -",
                 current_mood < 0 ? mood_names[q->mood] : mood_names[current_mood]);
        for (char *p = mood_tag; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        draw_text(81, tag_y, mood_tag, 170);
    }
}

/* ── TALL "longways" layout (122x250): 2-row status bar / quote / octopus
 * at the bottom. Pixel positions are first-pass and meant for on-device
 * tuning. ── */
void render_octopus_tall(const Quote *q, int expr, uint32_t frame_idx) {
    set_canvas_tall();          /* 122 x 250 */
    memset(frame, 0, sizeof(frame));

    /* ── Status bar (y0..24): wifi + bt + battery, then date/time ── */
    draw_wifi_icon(0, 1, wifi_connected);
    {
        int soc_x = 18;
        if (wetgreg_bt_state() == BT_PAIRED) { draw_bt_icon(18, 1); soc_x = 32; }
        if (wetgreg_social_active()) draw_social_icon(soc_x, 1);
    }
    draw_battery_icon(104, 1);
    {
        datetime_t t; rtc_get_datetime(&t);
        int hr12 = t.hour % 12; if (hr12 == 0) hr12 = 12;
        const char *ampm = (t.hour < 12) ? "AM" : "PM";
        char line[40];
        snprintf(line, sizeof(line), "%s %d  %d:%02d%s",
                 month_names[t.month - 1], t.day, hr12, t.min, ampm);
        int x = (122 - (int)strlen(line) * 6) / 2; if (x < 0) x = 0;
        draw_text(x, 14, line, 122);
    }
    for (int x = 4; x < 118; x++) px_set(x, 24);

    /* ── Quote bubble — dropped DOWN so its bottom sits just above the octopus
     *    head (~y125), with a speech-tail caret at the bottom-right so it reads
     *    like a quote. Text is CAPPED so it can't overrun the face. ── */
    {
        const int bx0 = 4, by0 = 48, bx1 = 117, by1 = 120;
        for (int x = bx0 + 2; x <= bx1 - 2; x++) { px_set(x, by0); px_set(x, by1); }
        for (int y = by0 + 2; y <= by1 - 2; y++) { px_set(bx0, y); px_set(bx1, y); }

        /* Speech-tail caret hanging off the bottom-right, pointing at the head. */
        int tipx = bx1 - 14, tipy = by1 + 9;
        for (int i = 0; i <= 12; i++) px_set(bx1 - 26 + i, by1 + (i * 9) / 12); /* left edge */
        for (int i = 0; i <= 9;  i++) px_set(bx1 - 8 - (i * 6) / 9, by1 + i);   /* right edge */
        px_set(tipx, tipy);
        for (int x = bx1 - 25; x < bx1 - 8; x++) px_clr(x, by1);                /* open the mouth */

        char qbuf[96];
        snprintf(qbuf, sizeof(qbuf), "%s", q->text);
        if (strlen(qbuf) > 90) { qbuf[88] = qbuf[89] = qbuf[90] = '.'; qbuf[91] = 0; }
        draw_text(bx0 + 4, by0 + 5, qbuf, (bx1 - bx0) - 8);
    }

    /* ── Octopus, moved down so the gaps above (bubble) and below (status
     *    block) are roughly even. Face lands ~y150. ── */
    const int OCT_OX = (122 - 65) / 2 - 5;
    const int OCT_OY = 113;
    layout_ox = OCT_OX;
    layout_oy = OCT_OY;
    draw_octopus(q, expr, frame_idx);
    layout_ox = 0; layout_oy = 0;

#if FACE_DEBUG
    /* TEMP: solid square at the computed left-eye position. If you SEE it on
     * the octopus face, the position is right and the eyes/mouth draw is the
     * bug; if you don't, the face is being clipped/placed off. */
    for (int yy = -3; yy <= 3; yy++)
        for (int xx = -3; xx <= 3; xx++)
            px_set(22 + OCT_OX + xx, 25 + Y_OFF + OCT_OY + yy);
#endif

    /* ── Bottom status block, pushed to the very bottom. Order (top→bottom):
     *    emotion state (centered), then STEPS (label left / count right). ── */
    {
        const int blk_div = 214, mood_y = 220, step_y = 234;
        for (int x = 4; x < 118; x++) px_set(x, blk_div);

        char mt[40];
        snprintf(mt, sizeof(mt), "- %s -",
                 current_mood < 0 ? mood_names[q->mood] : mood_names[current_mood]);
        for (char *p = mt; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        int mx = (122 - (int)strlen(mt) * 6) / 2; if (mx < 0) mx = 0;
        draw_text(mx, mood_y, mt, 122);          /* emotion, centered */

        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%lu", (unsigned long)steps_today);
        draw_text(4, step_y, "STEPS", 122);                      /* label left */
        int cx = 118 - (int)strlen(cnt) * 6; if (cx < 40) cx = 40;
        draw_text(cx, step_y, cnt, 122);                         /* count right */
    }
}

/* On-screen orientation HUD for calibration (compact: accel ×10, 0..3 hold).
 * Drawn last so it sits on top; gated by ORIENT_DEBUG. */
void draw_orient_hud(void) {
#if ORIENT_DEBUG
    char hud[28];
    snprintf(hud, sizeof(hud), "O%d X%d Y%d Z%d", g_orientation,
             (int)(accel_g(accel_x) * 10), (int)(accel_g(accel_y) * 10),
             (int)(accel_g(accel_z) * 10));
    /* In tall mode the top is the status bar — drop the HUD into the empty
     * band just below it so the date doesn't overlap it. */
    int hy = (canvas_w == 122) ? 40 : 2;
    draw_text(2, hy, hud, canvas_w);
#endif
}
