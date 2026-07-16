/**
 * screens_menu.c — the menu system + Settings-family screens.
 *
 * Purpose: render the main menu, the generic tall list/text helpers, and
 * every Settings-family screen.
 */
#include "screens_menu.h"

#include <stdio.h>
#include <string.h>

#include "accel.h"
#include "battery.h"
#include "canvas.h"
#include "display.h"      /* DISPLAY_NAME (info screen) */
#include "motion.h"
#include "octopus.h"      /* mood names + current_mood; QUOTE_COUNT via quotes.h */
#include "power.h"        /* g_batt_pct / g_batt_v (info screen) */
#include "rtc_clock.h"
#include "rtos_tasks.h"   /* display_render (mid-scan frame in render_i2c_scan) */
#include "speaker.h"
#include "text.h"
#include "version.h"
#include "wifi.h"

/* ─── Menu items ─── */
static const char *menu_items[] = {
    "MOOD SELECT",
    "ANIMATIONS",
    "SOUND",
    "MOTION",
    "DEVICE INFO",
    "SOCIAL",
    "SETTINGS",
    "BACK",
};

/* ─── Draw menu overlay ─── */
void render_menu(int selected) {
    /* Clear bottom half of screen for menu overlay */
    for (int y = 72; y < IMG_H; y++)
        for (int x = 0; x < IMG_ROW_BYTES; x++)
            frame[y * IMG_ROW_BYTES + x] = 0;

    for (int x = 5; x < 245; x++) px_set(x, 73);
    draw_text(8, 75, "MENU", IMG_W);

    /* 6 items — scroll if needed */
    int m_vis = 5;
    int m_start = 0;
    if (selected > m_vis - 2) m_start = selected - (m_vis - 2);
    if (m_start + m_vis > MENU_COUNT) m_start = MENU_COUNT - m_vis;
    if (m_start < 0) m_start = 0;
    for (int i = 0; i < m_vis && (m_start + i) < MENU_COUNT; i++) {
        int idx = m_start + i;
        int y = 84 + i * 8;
        char line[40];
        if (idx == selected) {
            snprintf(line, sizeof(line), "> %s", menu_items[idx]);
            draw_inverted_line(y, line);
        } else {
            snprintf(line, sizeof(line), "  %s", menu_items[idx]);
            draw_text(10, y, line, IMG_W);
        }
    }
}

/* ─── Menu redrawn + scaled for the TALL (122x250) orientation ───
 * A full vertical list (all items fit, no scrolling needed). */
void render_menu_tall(int selected) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));

    draw_text(44, 8, "MENU", 122);
    for (int x = 4; x < 118; x++) px_set(x, 20);

    for (int i = 0; i < MENU_COUNT; i++) {
        int y = 34 + i * 18;          /* roomy spacing in the tall view */
        char line[40];
        if (i == selected) {
            snprintf(line, sizeof(line), "> %s", menu_items[i]);
            draw_inverted_line(y, line);
        } else {
            snprintf(line, sizeof(line), "  %s", menu_items[i]);
            draw_text(8, y, line, 122);
        }
    }

    for (int x = 4; x < 118; x++) px_set(x, 236);
    draw_text(6, 240, "C:SELECT L:BACK", 122);
}

/* ─── Generic TALL (122x250) renderers, so every menu/list is readable and
 *     navigable when the device is held joystick-at-bottom. Input is already
 *     orientation-agnostic, so screens only need a tall render variant. ─── */
void render_list_tall(const char *title, const char *const *items,
                      int count, int sel, const char *footer) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));
    draw_text(6, 8, title, 122);
    for (int x = 4; x < 118; x++) px_set(x, 20);

    int vis = 9;                          /* ~9 rows fit (250px / 20) */
    int start = 0;
    if (sel > vis - 2) start = sel - (vis - 2);
    if (start + vis > count) start = count - vis;
    if (start < 0) start = 0;
    for (int i = 0; i < vis && (start + i) < count; i++) {
        int idx = start + i, y = 30 + i * 20;
        char line[44];
        if (idx == sel) {
            snprintf(line, sizeof(line), "> %s", items[idx]);
            draw_inverted_line(y, line);
        } else {
            snprintf(line, sizeof(line), "  %s", items[idx]);
            draw_text(8, y, line, 122);
        }
    }
    for (int x = 4; x < 118; x++) px_set(x, 232);
    draw_text(6, 236, footer ? footer : "C:SEL L:BACK", 122);
}

void render_text_tall(const char *title, const char *const *lines, int n) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));
    draw_text(6, 8, title, 122);
    for (int x = 4; x < 118; x++) px_set(x, 20);
    for (int i = 0; i < n; i++) draw_text(6, 30 + i * 16, lines[i], 122);
    draw_text(6, 236, "L:BACK", 122);
}

/* ─── Draw mood select screen ─── */
void render_mood_select(int selected) {
    if (orientation_is_tall()) {
        static const char *items[MOOD_COUNT + 1];
        items[0] = "ALL MOODS";
        for (int i = 0; i < MOOD_COUNT; i++) items[i + 1] = mood_names[i];
        render_list_tall("SELECT MOOD", items, MOOD_COUNT + 1, selected, "C:SET L:BACK");
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "SELECT MOOD", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    /* Show 8 moods at a time (scrolling window) */
    int start = 0;
    /* +1 for "ALL (RANDOM)" option */
    int total = MOOD_COUNT + 1;
    if (selected > 6) start = selected - 6;
    if (start + 8 > total) start = total - 8;
    if (start < 0) start = 0;

    for (int i = 0; i < 8 && (start + i) < total; i++) {
        int idx = start + i;
        int y = 20 + i * 11;
        char line[40];
        if (idx == 0) {
            snprintf(line, sizeof(line), idx == selected ? "> ALL MOODS (RANDOM)" : "  ALL MOODS (RANDOM)");
        } else {
            int mood_idx = idx - 1;
            snprintf(line, sizeof(line), idx == selected ? "> %s" : "  %s", mood_names[mood_idx]);
        }
        if (idx == selected)
            draw_inverted_line(y, line);
        else
            draw_text(10, y, line, IMG_W);
    }

    char hint[40];
    snprintf(hint, sizeof(hint), "CURRENT: %s",
             current_mood < 0 ? "ALL" : mood_names[current_mood]);
    draw_text(10, 110, hint, IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── Settings submenu (connectivity + display + time + factory reset) ─── */
static const char *set_items[SET_MENU_COUNT] = {
    "NETWORK", "BLUETOOTH", "DISPLAY", "SET TIME", "RESET WETGREG", "BACK",
};

void render_settings_menu(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 34, tall ? 10 : 3, "SETTINGS", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    int y = tall ? 34 : 22, dy = tall ? 22 : 14;
    for (int i = 0; i < SET_MENU_COUNT; i++) {
        int yy = y + i * dy; char l[36];
        if (i == sel) { snprintf(l, sizeof(l), "> %s", set_items[i]); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", set_items[i]); draw_text(8, yy, l, canvas_w); }
    }
    draw_text(8, tall ? 232 : 108, "C:SELECT  L:BACK", canvas_w);
}

/* ─── Display settings (auto-rotate + orientation) ─── */
static const char *orient_names[3] = {
    /* OR_LAND_R */ "WIDE",
    /* OR_LAND_L */ "WIDE FLIP",
    /* OR_TALL   */ "TALL",
};

void render_display_menu(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 36, tall ? 10 : 3, "DISPLAY", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    const char *items[DISP_MENU_COUNT];
    static char it_auto[24], it_orient[28];
    snprintf(it_auto, sizeof(it_auto), "AUTO-ROTATE: %s", g_auto_rotate ? "ON" : "OFF");
    /* Show the orientation that's actually active (auto follows the accel;
     * manual shows the locked choice). */
    int show_o = g_auto_rotate ? g_orientation : (int)g_manual_orient;
    if (show_o < 0 || show_o > 2) show_o = OR_TALL;
    snprintf(it_orient, sizeof(it_orient), "ORIENT: %s", orient_names[show_o]);
    items[DISP_ITEM_AUTO]   = it_auto;
    items[DISP_ITEM_ORIENT] = it_orient;
    items[DISP_ITEM_BACK]   = "BACK";

    int y = tall ? 34 : 24, dy = tall ? 22 : 14;
    for (int i = 0; i < DISP_MENU_COUNT; i++) {
        int yy = y + i * dy; char l[36];
        if (i == sel) { snprintf(l, sizeof(l), "> %s", items[i]); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", items[i]); draw_text(8, yy, l, canvas_w); }
    }
    /* Hint: ORIENT cycles WIDE → WIDE FLIP → TALL (and locks auto-rotate off). */
    draw_text(8, tall ? 232 : 108,
              sel == DISP_ITEM_ORIENT ? "C:CYCLE  L:BACK" : "C:TOGGLE  L:BACK", canvas_w);
}

/* ─── Animations submenu (intro story; future animations list here) ─── */
static const char *anim_items[ANIM_MENU_COUNT] = { "INTRO", "BACK" };

void render_anim_menu(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 30, tall ? 10 : 3, "ANIMATIONS", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    int y = tall ? 34 : 24, dy = tall ? 22 : 14;
    for (int i = 0; i < ANIM_MENU_COUNT; i++) {
        int yy = y + i * dy; char l[36];
        if (i == sel) { snprintf(l, sizeof(l), "> %s", anim_items[i]); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", anim_items[i]); draw_text(8, yy, l, canvas_w); }
    }
    draw_text(8, tall ? 232 : 108, "C:PLAY  L:BACK", canvas_w);
}

/* ─── Draw sound submenu ─── */
static const char *vol_labels[] = {"LOW", "MED", "HIGH"};

void render_sound_menu(int sel) {
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "SOUND", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[40];
    const char *items[SND_MENU_COUNT];
    static char pattern_buf[30];
    snprintf(pattern_buf, sizeof(pattern_buf), "TONE: %s",
             speaker_pattern_name(speaker_pattern()));
    items[SND_ITEM_PATTERN] = pattern_buf;
    static char onoff_buf[20];
    snprintf(onoff_buf, sizeof(onoff_buf), "SOUND: %s", speaker_enabled() ? "ON" : "OFF");
    items[SND_ITEM_ONOFF] = onoff_buf;
    static char vol_buf[20];
    snprintf(vol_buf, sizeof(vol_buf), "VOLUME: %s", vol_labels[speaker_volume()]);
    items[SND_ITEM_VOL] = vol_buf;
    items[SND_ITEM_BACK] = "BACK";

    if (orientation_is_tall()) {
        render_list_tall("SOUND", items, SND_MENU_COUNT, sel, "C:PLAY L:BACK");
        return;
    }

    for (int i = 0; i < SND_MENU_COUNT; i++) {
        int y = 22 + i * 12;
        if (i == sel) {
            snprintf(buf, sizeof(buf), "> %s", items[i]);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %s", items[i]);
            draw_text(10, y, buf, IMG_W);
        }
    }

    draw_text(10, 96, "C:PLAY  L/R:CHANGE TONE", IMG_W);
    draw_text(155, 108, "LEFT:BACK", IMG_W);
}

/* ─── Draw device info screen ─── */
void render_info_screen(void) {
    int   pct  = g_batt_pct;   /* filtered (batt_sample) — consistent with the icon */
    float vsys = g_batt_v;

    if (orientation_is_tall()) {
        static char L[9][40]; static const char *lp[9]; int n = 0;
        datetime_t t; rtc_get_datetime(&t);
        int h = t.hour % 12; if (h == 0) h = 12;
        snprintf(L[n], 40, "FW V%s", WETGREG_VERSION); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "DISP %s", DISPLAY_NAME); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "%s %d %d:%02d%s", month_names[t.month - 1], t.day,
                 h, t.min, t.hour < 12 ? "A" : "P"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "MOOD %s", current_mood < 0 ? "ALL" : mood_names[current_mood]); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "WIFI %s", wifi_connected ? wifi_ip_str : "OFF"); lp[n] = L[n]; n++;
        if (pct < 0) snprintf(L[n], 40, "USB %.2fV", (double)vsys);
        else         snprintf(L[n], 40, "BATT %d%% %.2fV", pct, (double)vsys);
        lp[n] = L[n]; n++;
        snprintf(L[n], 40, "CAL x%.3f", (double)g_vsys_cal); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "UP:CAL FULL=4.2"); lp[n] = L[n]; n++;
        render_text_tall("DEVICE INFO", lp, n);
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "DEVICE INFO", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);
    char buf[48];
    int y = 20;

    snprintf(buf, sizeof(buf), "FW: V%s  %s", WETGREG_VERSION, DISPLAY_NAME);
    draw_text(10, y, buf, IMG_W); y += 11;

    snprintf(buf, sizeof(buf), "BUILT: %s %s", __DATE__, __TIME__);
    draw_text(10, y, buf, IMG_W); y += 11;

    datetime_t t; rtc_get_datetime(&t);
    int hr12 = t.hour % 12; if (hr12 == 0) hr12 = 12;
    snprintf(buf, sizeof(buf), "%s %d, %d  %d:%02d %s",
             month_names[t.month - 1], t.day, t.year, hr12, t.min,
             t.hour < 12 ? "AM" : "PM");
    draw_text(10, y, buf, IMG_W); y += 11;

    snprintf(buf, sizeof(buf), "MOOD: %s  QUOTES: %d",
             current_mood < 0 ? "ALL" : mood_names[current_mood], QUOTE_COUNT);
    draw_text(10, y, buf, IMG_W); y += 11;

    snprintf(buf, sizeof(buf), "WIFI: %s", wifi_connected ? wifi_ip_str : "OFF");
    draw_text(10, y, buf, IMG_W); y += 11;

    /* Battery / power status — show the live voltage to 2 decimals + cal trim. */
    if (pct < 0) {
        snprintf(buf, sizeof(buf), "POWER: USB  %.2fV  (CAL x%.3f)",
                 (double)vsys, (double)g_vsys_cal);
    } else {
        snprintf(buf, sizeof(buf), "BATTERY: %d%%  %.2fV  (CAL x%.3f)",
                 pct, (double)vsys, (double)g_vsys_cal);
    }
    draw_text(10, y, buf, IMG_W); y += 11;

    draw_text(10, 100, "UP:CAL FULL=4.2V  DN:RESET CAL", IMG_W);
    draw_text(10, 112, "PICO 2 W  RP2350        LEFT:BACK", IMG_W);
}

/* ─── Manual date/time setter ─── */
datetime_t settime_dt;
int  settime_field   = 0;     /* 0=year 1=month 2=day 3=hour 4=min */
bool settime_editing = false; /* false = pick a field, true = adjust its value */

void render_set_time(void) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));

    draw_text(tall ? 8 : 30, tall ? 10 : 3, "SET DATE / TIME", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    char f[5][14];
    snprintf(f[0], 14, "YEAR  %d", settime_dt.year);
    snprintf(f[1], 14, "MONTH %02d", settime_dt.month);
    snprintf(f[2], 14, "DAY   %02d", settime_dt.day);
    snprintf(f[3], 14, "HOUR  %02d", settime_dt.hour);
    snprintf(f[4], 14, "MIN   %02d", settime_dt.min);

    int y0 = tall ? 44 : 26, dy = tall ? 22 : 13;
    for (int i = 0; i < 5; i++) {
        int y = y0 + i * dy;
        bool selected = (i == settime_field);
        /* Selected field is a steady highlight either way. EDIT mode adds a second
         * '>' arrow so it's clear U/D now adjusts the value (vs. just picking it). */
        const char *prefix = selected ? (settime_editing ? ">>" : "> ") : "  ";
        char line[20];
        snprintf(line, sizeof(line), "%s%s", prefix, f[i]);
        if (selected) draw_inverted_line(y, line);
        else          draw_text(8, y, line, canvas_w);
    }
    draw_text(4, tall ? 232 : 108,
              settime_editing ? "U/D ADJUST  C OK  L BACK"
                              : "U/D FIELD  C EDIT  L BACK", canvas_w);
}

/* ─── Factory reset confirmation (Settings → RESET WETGREG) ─── */
void render_reset_confirm(void) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 30, tall ? 10 : 3, "RESET WETGREG?", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    /* Lines kept ≤19 chars so nothing wraps on the 122 px tall canvas. */
    int y = tall ? 40 : 24, dy = tall ? 20 : 13;
    draw_text(8, y, "ERASES ALL SETTINGS", canvas_w); y += dy;
    draw_text(8, y, "AND PROGRESS:", canvas_w);       y += dy + (tall ? 6 : 2);
    draw_text(8, y, "- SAVED WIFI", canvas_w);        y += dy;
    draw_text(8, y, "- NAME + FRIENDS", canvas_w);    y += dy;
    draw_text(8, y, "- DISPLAY + SOCIAL", canvas_w);  y += dy;
    draw_text(8, y, "THEN REBOOTS AS NEW", canvas_w);
    draw_text(8, tall ? 232 : 108, "C:RESET  L:CANCEL", canvas_w);
}

/* ─── Draw motion / accelerometer menu ─── */

/* Tall layout: the 122px width can't fit "ACCEL: x y zG" on one line, so the
 * three read-out items put their value on its OWN line under the label. */
static void render_motion_tall(int sel) {
    set_canvas_tall();
    memset(frame, 0, sizeof(frame));
    draw_text(8, 10, "MOTION", canvas_w);
    for (int x = 4; x < 118; x++) px_set(x, 22);

    static char vacc[24], vped[24], vtil[24], vthr[20];
    snprintf(vacc, sizeof(vacc), " %.1f %.1f %.1fG",
             (double)accel_g(accel_x), (double)accel_g(accel_y), (double)accel_g(accel_z));
    snprintf(vped, sizeof(vped), " %lu ST %ld MIN",
             (unsigned long)steps_today, (long)(active_seconds_today / 60));
    snprintf(vtil, sizeof(vtil), " X%.0f Y%.0f %.1fC",
             (double)tilt_x_deg(), (double)tilt_y_deg(), (double)mpu_temp_c);
    snprintf(vthr, sizeof(vthr), "THRESHOLD: %.1fG", (double)step_threshold);

    const char *labels[MOT_MENU_COUNT] = {
        "ACCEL:", "TODAY:", "TILT:", "RESET PEDOMETER", vthr, "I2C BUS SCAN", "BACK",
    };
    const char *vals[MOT_MENU_COUNT] = { vacc, vped, vtil, NULL, NULL, NULL, NULL };

    int y = 30, dy = 16;
    for (int i = 0; i < MOT_MENU_COUNT; i++) {
        char line[28];
        if (i == sel) { snprintf(line, sizeof(line), "> %s", labels[i]); draw_inverted_line(y, line); }
        else          { snprintf(line, sizeof(line), "  %s", labels[i]); draw_text(8, y, line, canvas_w); }
        y += dy;
        if (vals[i]) { draw_text(8, y, vals[i], canvas_w); y += dy; }
    }
    draw_text(6, 236, "C:SEL  L:BACK", canvas_w);
}

void render_motion_menu(int sel) {
    /* Phase 2: the Housekeeping task already samples the accelerometer (~20 Hz)
     * and runs the pedometer, so we read the shared accel/step globals it keeps
     * fresh. Calling mpu_read_all()/pedometer_update() here too would DOUBLE-COUNT
     * steps (HK + UI both crossing the threshold) — so it's removed. */
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "MOTION", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[42];
    const char *items[MOT_MENU_COUNT];

    static char live_buf[42];
    snprintf(live_buf, sizeof(live_buf), "ACCEL: %.1f  %.1f  %.1fG",
             (double)accel_g(accel_x), (double)accel_g(accel_y), (double)accel_g(accel_z));
    items[MOT_ITEM_LIVE] = live_buf;

    static char ped_buf[42];
    snprintf(ped_buf, sizeof(ped_buf), "TODAY: %lu ST  %ld MIN",
             (unsigned long)steps_today, (long)(active_seconds_today / 60));
    items[MOT_ITEM_PEDOMETER] = ped_buf;

    static char tilt_buf[30];
    snprintf(tilt_buf, sizeof(tilt_buf), "TILT: X%.0f  Y%.0f  %.1fC",
             (double)tilt_x_deg(), (double)tilt_y_deg(), (double)mpu_temp_c);
    items[MOT_ITEM_TILT] = tilt_buf;

    items[MOT_ITEM_RESET] = "RESET PEDOMETER";

    static char thresh_buf[25];
    snprintf(thresh_buf, sizeof(thresh_buf), "THRESHOLD: %.1fG", (double)step_threshold);
    items[MOT_ITEM_THRESH] = thresh_buf;

    items[MOT_ITEM_I2CSCAN] = "I2C BUS SCAN";
    items[MOT_ITEM_BACK] = "BACK";

    if (orientation_is_tall()) {
        render_motion_tall(sel);
        return;
    }

    /* Scrolling window — 5 items visible */
    int vis = 5;
    int start = 0;
    if (sel > vis - 2) start = sel - (vis - 2);
    if (start + vis > MOT_MENU_COUNT) start = MOT_MENU_COUNT - vis;
    if (start < 0) start = 0;

    for (int i = 0; i < vis && (start + i) < MOT_MENU_COUNT; i++) {
        int idx = start + i;
        int y = 18 + i * 11;
        if (idx == sel) {
            snprintf(buf, sizeof(buf), "> %s", items[idx]);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %s", items[idx]);
            draw_text(10, y, buf, IMG_W);
        }
    }

    /* Scroll indicators */
    if (start > 0) draw_text(235, 18, "/", IMG_W);
    if (start + vis < MOT_MENU_COUNT) draw_text(235, 18 + (vis - 1) * 11, "/", IMG_W);

    /* Gyro readout at bottom */
    static char gyro_buf[42];
    snprintf(gyro_buf, sizeof(gyro_buf), "GYRO: %.0f  %.0f  %.0f D/S",
             (double)gyro_dps(gyro_x), (double)gyro_dps(gyro_y), (double)gyro_dps(gyro_z));
    draw_text(10, 86, gyro_buf, IMG_W);

    /* Magnitude bar */
    float mag = accel_magnitude();
    snprintf(buf, sizeof(buf), "MAG: %.2fG", (double)mag);
    draw_text(10, 98, buf, IMG_W);

    if (!mpu_ok) draw_text(130, 98, "MPU:NO", IMG_W);
    else draw_text(130, 98, "MPU:OK", IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── I2C bus scan screen ─── */
void render_i2c_scan(void) {
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "I2C BUS SCAN", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    draw_text(10, 20, "SCANNING I2C0 (GP0/GP1)...", IMG_W);
    transpose_to_display();
    display_render();

    /* Scan all valid 7-bit addresses */
    uint8_t found[16];
    int found_count = 0;
    printf("[I2C] Scanning I2C0 bus...\n");

    for (int addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy;
        int ret = i2c_read_timeout_us(MPU_I2C, addr, &dummy, 1, false, MPU_I2C_TIMEOUT_US);
        if (ret >= 0 && found_count < 16) {
            found[found_count++] = (uint8_t)addr;
            printf("[I2C]   Device at 0x%02X\n", addr);
        }
    }
    printf("[I2C] Scan complete: %d device(s)\n", found_count);

    /* Show results */
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "I2C BUS SCAN", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[42];
    snprintf(buf, sizeof(buf), "FOUND %d DEVICE(S):", found_count);
    draw_text(10, 20, buf, IMG_W);

    for (int i = 0; i < found_count && i < 8; i++) {
        const char *desc = "";
        if (found[i] == 0x68) desc = " (MPU-6050)";
        else if (found[i] == 0x69) desc = " (MPU-6050 AD0:H)";
        else if (found[i] == 0x76) desc = " (BME280/BMP280)";
        else if (found[i] == 0x77) desc = " (BME280/BMP280)";
        else if (found[i] == 0x3C) desc = " (SSD1306 OLED)";
        else if (found[i] == 0x50) desc = " (EEPROM)";
        snprintf(buf, sizeof(buf), "  0X%02X%s", found[i], desc);
        draw_text(10, 32 + i * 10, buf, IMG_W);
    }

    if (found_count == 0) {
        draw_text(10, 40, "NO DEVICES FOUND", IMG_W);
        draw_text(10, 55, "CHECK WIRING:", IMG_W);
        draw_text(10, 66, "SDA:GP0(PIN1) SCL:GP1(PIN2)", IMG_W);
        draw_text(10, 77, "VCC:3V3 GND:GND AD0:GND", IMG_W);
    }

    draw_text(155, 110, "C:BACK", IMG_W);
}
