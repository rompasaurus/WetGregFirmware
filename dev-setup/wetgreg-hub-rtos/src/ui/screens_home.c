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
#include "storage.h"     /* lang_is_ar */
#include "text.h"
#include "text_ar.h"
#include "wifi.h"

/* Egyptian-Arabic month names (UTF-8), the forms used in Egypt. */
static const char *const month_names_ar[12] = {
    "يناير", "فبراير", "مارس", "أبريل", "مايو", "يونيو",
    "يوليو", "أغسطس", "سبتمبر", "أكتوبر", "نوفمبر", "ديسمبر"
};

/* "٣:٤٧ م" style time in Arabic-Indic digits; ص = AM, م = PM. */
static void ar_time(char *buf, int n, const datetime_t *t) {
    int hr12 = t->hour % 12; if (hr12 == 0) hr12 = 12;
    char h[8], m[8], mm[8];
    ar_itoa(h, sizeof(h), (unsigned)hr12);
    ar_itoa(m, sizeof(m), (unsigned)t->min);
    if (t->min < 10) { mm[0] = (char)0xD9; mm[1] = (char)0xA0; strcpy(mm + 2, m); }   /* leading ٠ */
    else strcpy(mm, m);
    snprintf(buf, n, "%s:%s %s", h, mm, t->hour < 12 ? "ص" : "م");
}

/* TEMP debug: draw a marker square at the computed left-eye position in the
 * tall layout, to separate face-position bugs from face-draw bugs. */
#define FACE_DEBUG 0

void draw_clock_header(void) {
    datetime_t t;
    rtc_get_datetime(&t);

    if (lang_is_ar()) {
        /* "١٢ أبريل ٢٠٢٦  ٣:٤٧ م" — the Kufi cell is 17 rows, so start it
         * above the canvas: body rows land on y 2..11, clear of the bubble. */
        char d[8], y[12], tm[24], buf[64];
        ar_itoa(d, sizeof(d), (unsigned)t.day);
        ar_itoa(y, sizeof(y), (unsigned)t.year);
        ar_time(tm, sizeof(tm), &t);
        snprintf(buf, sizeof(buf), "%s %s %s  %s", d, month_names_ar[t.month - 1], y, tm);
        draw_text_ar_centered(0, IMG_W, -3, buf);
        return;
    }

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

/* ─── Chat bubble (wide layout); bh = box height (70 Latin, taller for Arabic) ─── */
static void draw_bubble(int bh) {
    int bx = 75, by = 5 + Y_OFF, bw = 170;
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
    int mood = current_mood < 0 ? q->mood : current_mood;
    if (lang_is_ar()) {
        /* Taller bubble (y 17..90) so 4 lines of the 13 px quote font fit:
         * cells at 20, 37, 54, 71 → last body row 84, descenders 88.
         * Right-aligned at the bubble's inner edge (x 239). */
        draw_bubble(74);
        draw_text_ar(239, 8 + Y_OFF, q->text, 158, 4);
        /* Mood tag under the bubble in the quote font: cell top 91 → body
         * 95..104, descenders 108 — clear of the STEPS/DOWN:MENU row the app
         * draws at the bottom (body 112..121). */
        int tag_y = 5 + Y_OFF + 74;
        char tag[48];
        snprintf(tag, sizeof(tag), "- %s -", mood_names_ar[mood]);
        draw_text_ar_f(AR_FONT_Q, 239, tag_y, tag, 170, 1, 0);   /* quote-size tag */
        return;
    }
    draw_bubble(70);
    draw_text(81, 11 + Y_OFF, q->text, 158);

    /* Tagline — current mood/emotion name */
    int tag_y = 5 + 70 + 5 + Y_OFF;
    if (tag_y + 7 < IMG_H) {
        char mood_tag[40];
        snprintf(mood_tag, sizeof(mood_tag), "- %s -", mood_names[mood]);
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
    const bool ar = lang_is_ar();
    {
        datetime_t t; rtc_get_datetime(&t);
        if (ar) {
            /* "١٢ أبريل ٣:٤٧ م" in the UI font (11 px: the quote font would
             * overflow 122 px on long months): cell top 5 → body on y 10..19,
             * descenders just reach the divider at 24. */
            char d[8], tm[24], line[48];
            ar_itoa(d, sizeof(d), (unsigned)t.day);
            ar_time(tm, sizeof(tm), &t);
            snprintf(line, sizeof(line), "%s %s %s", d, month_names_ar[t.month - 1], tm);
            int tw = text_ar_width_f(AR_FONT_UI, line);
            draw_text_ar_f(AR_FONT_UI, (122 + tw) / 2, 5, line, 122, 1, 0);
        } else {
            int hr12 = t.hour % 12; if (hr12 == 0) hr12 = 12;
            const char *ampm = (t.hour < 12) ? "AM" : "PM";
            char line[40];
            snprintf(line, sizeof(line), "%s %d  %d:%02d%s",
                     month_names[t.month - 1], t.day, hr12, t.min, ampm);
            if ((int)strlen(line) * 6 > 122)   /* long month overflows 122 px — abbreviate */
                snprintf(line, sizeof(line), "%.3s %d  %d:%02d%s",
                         month_names[t.month - 1], t.day, hr12, t.min, ampm);
            int x = (122 - (int)strlen(line) * 6) / 2; if (x < 0) x = 0;
            draw_text(x, 14, line, 122);
        }
    }
    for (int x = 4; x < 118; x++) px_set(x, 24);

    /* ── Quote bubble — dropped DOWN so its bottom sits just above the octopus
     *    head (~y125), with a speech-tail caret at the bottom-right so it reads
     *    like a quote. Text is CAPPED so it can't overrun the face. Arabic
     *    lines are 17 px tall, so the bubble takes the full width and grows
     *    upward into the empty band under the status bar to keep 5 lines
     *    (every quote in quotes_ar.h fits 5 lines at 111 px — checked). ── */
    {
        const int bx0 = ar ? 1 : 4, by0 = ar ? 28 : 48, bx1 = ar ? 120 : 117, by1 = 120;
        for (int x = bx0 + 2; x <= bx1 - 2; x++) { px_set(x, by0); px_set(x, by1); }
        for (int y = by0 + 2; y <= by1 - 2; y++) { px_set(bx0, y); px_set(bx1, y); }

        /* Speech-tail caret hanging off the bottom-right, pointing at the head. */
        int tipx = bx1 - 14, tipy = by1 + 9;
        for (int i = 0; i <= 12; i++) px_set(bx1 - 26 + i, by1 + (i * 9) / 12); /* left edge */
        for (int i = 0; i <= 9;  i++) px_set(bx1 - 8 - (i * 6) / 9, by1 + i);   /* right edge */
        px_set(tipx, tipy);
        for (int x = bx1 - 25; x < bx1 - 8; x++) px_clr(x, by1);                /* open the mouth */

        if (ar) {
            /* 5 lines × 17 px from y 31 → cells 31..99; the last line's body
             * ends at 112 and its descenders at 116, inside the box (120). */
            draw_text_ar(bx1 - 3, by0 + 3, q->text, (bx1 - bx0) - 8, 5);
        } else {
            char qbuf[96];
            snprintf(qbuf, sizeof(qbuf), "%s", q->text);
            if (strlen(qbuf) > 90) { qbuf[88] = qbuf[89] = qbuf[90] = '.'; qbuf[91] = 0; }
            draw_text(bx0 + 4, by0 + 5, qbuf, (bx1 - bx0) - 8);
        }
    }

    /* ── Octopus, moved down so the gaps above (bubble) and below (status
     *    block) are roughly even. Face lands ~y150. ── */
    const int OCT_OX = (122 - 65) / 2 - 5;
    const int OCT_OY = ar ? 109 : 113;    /* Arabic: 4 px higher → room for the 18 px rows below */
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
        /* Arabic uses the 18 px headline font for both rows; the octopus sits
         * 4 px higher there (art ends ~201) and the divider moves up to 205. */
        const int blk_div = ar ? 205 : 214, mood_y = 220, step_y = 234;
        for (int x = 4; x < 118; x++) px_set(x, blk_div);

        int mood = current_mood < 0 ? q->mood : current_mood;
        if (ar) {
            /* 18 px cells are 26 rows with the body on rows 5..18 and
             * descenders to 22: mood cell top 203 → body 208..221, descenders
             * 225; steps cell top 227 → body 232..245, descenders 249. No
             * dashes around the mood: "واحشني البيت" alone is 104 of 122 px. */
            char mt[48], cnt[16];
            snprintf(mt, sizeof(mt), "%s", mood_names_ar[mood]);
            int mw = text_ar_width_f(AR_FONT_BIG, mt);
            draw_text_ar_f(AR_FONT_BIG, (122 + mw) / 2, 203, mt, 122, 1, 0);
            ar_itoa(cnt, sizeof(cnt), (unsigned)steps_today);
            draw_text_ar_f(AR_FONT_BIG, 118, 227, "خطوات", 70, 1, 0);               /* label right */
            draw_text_ar_f(AR_FONT_BIG, 4 + text_ar_width_f(AR_FONT_BIG, cnt), 227, cnt, 60, 1, 0); /* count left */
        } else {
            char mt[40];
            snprintf(mt, sizeof(mt), "- %s -", mood_names[mood]);
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
