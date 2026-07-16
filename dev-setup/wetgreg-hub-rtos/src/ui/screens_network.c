/**
 * screens_network.c — connectivity screens (WiFi + Bluetooth).
 *
 * Purpose: render the WiFi status/menu/scan/keyboard/saved-networks screens
 * and the Bluetooth pairing screen.
 */
#include "screens_network.h"

#include <stdio.h>
#include <string.h>

#include "bt.h"
#include "canvas.h"
#include "display.h"      /* transpose_to_display (connecting interstitial) */
#include "motion.h"
#include "ntp.h"
#include "rtos_tasks.h"   /* display_render */
#include "screens_menu.h" /* render_list_tall / render_text_tall */
#include "storage.h"
#include "text.h"
#include "wifi.h"

/* ─── Draw network status screen (read-only) ─── */
void render_network_screen(void) {
    if (orientation_is_tall()) {
        static char L[6][40]; static const char *lp[6]; int n = 0;
        snprintf(L[n], 40, "WIFI: %s", wifi_enabled ? "ON" : "OFF"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "SSID: %s", wifi_ssid_display); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "STATE: %s", !wifi_enabled ? "OFF" :
                 wifi_connected ? "CONNECTED" : "DISCONN"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "IP: %s", wifi_connected ? wifi_ip_str : "---"); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "SIGNAL: %d dBm", wifi_connected ? (int)wifi_rssi : 0); lp[n] = L[n]; n++;
        snprintf(L[n], 40, "NTP: %s", ntp_synced ? "SYNCED" : "NOT SYNCED"); lp[n] = L[n]; n++;
        render_text_tall("WIFI STATUS", lp, n);
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "WIFI STATUS", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);
    char buf[48];

    snprintf(buf, sizeof(buf), "WIFI: %s", wifi_enabled ? "ON" : "OFF");
    draw_text(10, 22, buf, IMG_W);

    snprintf(buf, sizeof(buf), "SSID: %s", wifi_ssid_display);
    draw_text(10, 35, buf, IMG_W);

    snprintf(buf, sizeof(buf), "STATUS: %s",
             !wifi_enabled ? "DISABLED" :
             wifi_connected ? "CONNECTED" : "DISCONNECTED");
    draw_text(10, 48, buf, IMG_W);

    snprintf(buf, sizeof(buf), "IP: %s", wifi_connected ? wifi_ip_str : "---");
    draw_text(10, 61, buf, IMG_W);

    if (wifi_connected) {
        snprintf(buf, sizeof(buf), "SIGNAL: %d DBM", (int)wifi_rssi);
        draw_text(10, 74, buf, IMG_W);
    }

    snprintf(buf, sizeof(buf), "NTP: %s", ntp_synced ? "SYNCED" : "NOT SYNCED");
    draw_text(10, 87, buf, IMG_W);

    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── Draw network submenu ─── */
void render_net_menu(int sel) {
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "NETWORK", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    char buf[40];
    const char *items[NET_MENU_COUNT];
    static char onoff_buf[20];
    snprintf(onoff_buf, sizeof(onoff_buf), "WIFI: %s", wifi_enabled ? "ON" : "OFF");
    items[NET_ITEM_ONOFF] = onoff_buf;
    items[NET_ITEM_SCAN] = "SCAN NETWORKS";
    items[NET_ITEM_SAVED] = "SAVED NETWORKS";
    items[NET_ITEM_STATUS] = "STATUS";
    items[NET_ITEM_BACK] = "BACK";

    if (orientation_is_tall()) {
        render_list_tall("NETWORK", items, NET_MENU_COUNT, sel, "C:SEL L:BACK");
        return;
    }

    for (int i = 0; i < NET_MENU_COUNT; i++) {
        int y = 22 + i * 12;
        if (i == sel) {
            snprintf(buf, sizeof(buf), "> %s", items[i]);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %s", items[i]);
            draw_text(10, y, buf, IMG_W);
        }
    }

    snprintf(buf, sizeof(buf), "%s", wifi_connected ? "CONNECTED" : "DISCONNECTED");
    draw_text(10, 100, buf, IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── Draw scan results ─── */
void render_scan_results(void) {
    if (orientation_is_tall()) {
        if (scan_in_progress || scan_count == 0) {
            set_canvas_tall(); memset(frame, 0, sizeof(frame));
            draw_text(6, 8, "WIFI NETWORKS", 122);
            for (int x = 4; x < 118; x++) px_set(x, 20);
            draw_text(8, 60, scan_in_progress ? "SCANNING..." : "NO NETWORKS", 122);
            draw_text(6, 236, scan_in_progress ? "L:CANCEL" : "L:BACK", 122);
            return;
        }
        static char rows[MAX_SCAN_RESULTS][26];
        static const char *items[MAX_SCAN_RESULTS];
        for (int i = 0; i < scan_count; i++) {
            char lock = (scan_results[i].auth_mode != 0) ? '~' : ' ';
            snprintf(rows[i], sizeof(rows[i]), "%c%-11.11s%ddB",
                     lock, scan_results[i].ssid, (int)scan_results[i].rssi);
            items[i] = rows[i];
        }
        render_list_tall("WIFI NETWORKS", items, scan_count, scan_sel, "C:JOIN L:BACK");
        return;
    }
    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "WIFI NETWORKS", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);

    if (scan_in_progress) {
        draw_text(60, 50, "SCANNING...", IMG_W);
        char buf[20];
        snprintf(buf, sizeof(buf), "FOUND: %d", scan_count);
        draw_text(80, 65, buf, IMG_W);
        draw_text(175, 110, "LEFT:CANCEL", IMG_W);
        return;
    }

    if (scan_count == 0) {
        draw_text(50, 55, "NO NETWORKS FOUND", IMG_W);
        draw_text(175, 110, "LEFT:BACK", IMG_W);
        return;
    }

    /* Scrolling list — 7 items visible */
    int start = 0;
    if (scan_sel > 5) start = scan_sel - 5;
    if (start + 7 > scan_count) start = scan_count - 7;
    if (start < 0) start = 0;

    char buf[42];
    for (int i = 0; i < 7 && (start + i) < scan_count; i++) {
        int idx = start + i;
        int y = 20 + i * 12;
        char lock = (scan_results[idx].auth_mode != 0) ? '~' : ' ';
        /* SSID truncated to a fixed field so the signal (dBm) lines up; list
           is sorted strongest-first. */
        if (idx == scan_sel) {
            snprintf(buf, sizeof(buf), "> %c%-22.22s %ddBm", lock,
                     scan_results[idx].ssid, (int)scan_results[idx].rssi);
            draw_inverted_line(y, buf);
        } else {
            snprintf(buf, sizeof(buf), "  %c%-22.22s %ddBm", lock,
                     scan_results[idx].ssid, (int)scan_results[idx].rssi);
            draw_text(10, y, buf, IMG_W);
        }
    }

    snprintf(buf, sizeof(buf), "%d FOUND  C:CONNECT", scan_count);
    draw_text(10, 110, buf, IMG_W);
    draw_text(175, 110, "LEFT:BACK", IMG_W);
}

/* ─── On-screen keyboard state ─── */
char pw_buf[PW_MAX_LEN + 1];
int  pw_len = 0;
int  kb_row = 0;
int  kb_col = 0;
bool kb_shift = true;   /* start in CAPS mode */
int  selected_network = -1;

static const char kb_grid[4][10] = {
    {'Q','W','E','R','T','Y','U','I','O','P'},
    {'A','S','D','F','G','H','J','K','L','.'},
    {'Z','X','C','V','B','N','M','-','!','?'},
    {'0','1','2','3','4','5','6','7','8','9'},
};

char kb_char_at(int row, int col, bool shift) {
    if (row >= KB_CHAR_ROWS) return '\0';
    char c = kb_grid[row][col];
    if (!shift && c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/* ─── Draw on-screen keyboard ─── */
void render_keyboard(void) {
    memset(frame, 0, sizeof(frame));

    /* Header: SSID */
    char hdr[42];
    snprintf(hdr, sizeof(hdr), "CONNECT: %s",
             selected_network >= 0 ? scan_results[selected_network].ssid : "?");
    draw_text(5, 0, hdr, IMG_W);

    /* Password field + shift indicator */
    char pw_show[35];
    int vis_start = pw_len > 28 ? pw_len - 28 : 0;
    for (int i = 0; i < pw_len - vis_start; i++) {
        char c = pw_buf[vis_start + i];
        pw_show[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    pw_show[pw_len - vis_start] = '\0';
    char pw_line[42];
    snprintf(pw_line, sizeof(pw_line), "PW:%s %s", pw_show, kb_shift ? "(CAPS)" : "(LOW)");
    draw_text(5, 10, pw_line, IMG_W);

    /* Separator */
    for (int x = 5; x < 245; x++) px_set(x, 20);

    /* Character grid: 4 rows x 10 cols, cells 24px wide x 12px tall */
    for (int r = 0; r < KB_CHAR_ROWS; r++) {
        for (int c = 0; c < 10; c++) {
            int cx = 5 + c * 24;
            int cy = 24 + r * 12;
            char ch = kb_grid[r][c];

            if (kb_row == r && kb_col == c) {
                /* Selected: inverted cell */
                for (int iy = cy; iy < cy + 10; iy++)
                    for (int ix = cx; ix < cx + 22; ix++)
                        px_set(ix, iy);
                /* Draw character white-on-black */
                const uint8_t *g = font_glyph(ch);
                if (g) {
                    for (int row2 = 0; row2 < 7; row2++) {
                        uint8_t bits = g[row2];
                        for (int col2 = 0; col2 < 5; col2++)
                            if (bits & (0x10 >> col2))
                                px_clr(cx + 8 + col2, cy + 1 + row2);
                    }
                }
            } else {
                /* Normal cell */
                draw_char_c(cx + 8, cy + 1, ch);
            }
        }
    }

    /* Special keys row at y=76 */
    static const char *sp_labels[] = {"SHIFT", "SPC", "DEL", "DONE", "CANCEL"};
    int sp_x[] = {5, 50, 90, 135, 190};
    int sp_w[] = {40, 35, 40, 50, 55};
    for (int i = 0; i < KB_SP_COUNT; i++) {
        int sx = sp_x[i];
        int sy = 76;
        if (kb_row == KB_SPECIAL_ROW && kb_col == i) {
            for (int iy = sy; iy < sy + 10; iy++)
                for (int ix = sx; ix < sx + sp_w[i]; ix++)
                    px_set(ix, iy);
            /* Draw label white-on-black */
            int tx = sx + 3;
            for (const char *cp = sp_labels[i]; *cp; cp++) {
                const uint8_t *g = font_glyph(*cp);
                if (g) {
                    for (int row2 = 0; row2 < 7; row2++) {
                        uint8_t bits = g[row2];
                        for (int col2 = 0; col2 < 5; col2++)
                            if (bits & (0x10 >> col2))
                                px_clr(tx + col2, sy + 1 + row2);
                    }
                }
                tx += 6;
            }
        } else {
            draw_text(sx + 3, sy + 1, sp_labels[i], sp_w[i]);
        }
    }

    /* Help text */
    draw_text(5, 108, "U/D/L/R:MOVE  C:SELECT", IMG_W);
}

/* ─── Saved networks screen (connect / forget) ─── */
int saved_sel = 0;

void render_saved_nets(void) {
    if (g_saved.count == 0) saved_sel = 0;
    else if (saved_sel >= (int)g_saved.count) saved_sel = g_saved.count - 1;

    if (orientation_is_tall()) {
        static const char *items[MAX_SAVED];
        for (uint32_t i = 0; i < g_saved.count; i++) items[i] = g_saved.nets[i].ssid;
        if (g_saved.count == 0) {
            set_canvas_tall(); memset(frame, 0, sizeof(frame));
            draw_text(6, 8, "SAVED NETWORKS", 122);
            draw_text(8, 60, "NONE SAVED", 122);
            draw_text(6, 236, "L:BACK", 122);
            return;
        }
        render_list_tall("SAVED NETWORKS", items, g_saved.count, saved_sel,
                         "C:JOIN R:FORGET L:BACK");
        return;
    }

    memset(frame, 0, sizeof(frame));
    draw_text(30, 3, "SAVED NETWORKS", IMG_W);
    for (int x = 10; x < 240; x++) px_set(x, 14);
    if (g_saved.count == 0) {
        draw_text(50, 55, "NONE SAVED", IMG_W);
    } else {
        char line[40];
        for (uint32_t i = 0; i < g_saved.count && i < 7; i++) {
            int y = 22 + i * 12;
            if ((int)i == saved_sel) {
                snprintf(line, sizeof(line), "> %s", g_saved.nets[i].ssid);
                draw_inverted_line(y, line);
            } else {
                snprintf(line, sizeof(line), "  %s", g_saved.nets[i].ssid);
                draw_text(10, y, line, IMG_W);
            }
        }
    }
    draw_text(8, 110, "C:JOIN  R:FORGET  LEFT:BACK", IMG_W);
}

/* ─── Show connecting screen (blocks during wifi_connect_to) ─── */
void show_connecting_screen(const char *ssid) {
    memset(frame, 0, sizeof(frame));
    draw_text(40, 40, "CONNECTING TO", IMG_W);
    draw_text(40, 55, ssid, IMG_W);
    draw_text(40, 75, "PLEASE WAIT...", IMG_W);
    transpose_to_display();
    display_render();
}

/* ─── Bluetooth pairing screen ─── */
void render_bluetooth(bool enabled) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 6 : 30, tall ? 8 : 3, "BLUETOOTH", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 20 : 14);

    char line[40];

    /* ── Pairing: show the 6-digit passkey big; phone enters it. ── */
    if (wetgreg_bt_state() == BT_PAIRING) {
        int y = tall ? 44 : 26, dy = tall ? 20 : 14;
        draw_text(8, y, "ENTER THIS CODE ON", canvas_w); y += dy;
        draw_text(8, y, "YOUR PHONE:", canvas_w); y += dy + dy;
        snprintf(line, sizeof(line), "%06lu", (unsigned long)wetgreg_bt_passkey());
        /* double-size centred passkey */
        int tw = (int)strlen(line) * 16;
        draw_text_2x((canvas_w - tw) / 2, y, line);
        draw_text(tall ? 6 : 8, tall ? 236 : 110, "LEFT:CANCEL", canvas_w);
        return;
    }

    int y = tall ? 36 : 26, dy = tall ? 18 : 13;
    draw_text(8, y, "NAME: WetGreg Hub", canvas_w); y += dy;

    /* ── Disabled: radio is off to save power. ── */
    if (!enabled) {
        draw_text(8, y, "STATUS: OFF (DISABLED)", canvas_w); y += dy + dy;
        draw_text(8, y, "BLUETOOTH IS OFF TO", canvas_w); y += dy;
        draw_text(8, y, "SAVE POWER.", canvas_w);
        draw_text(tall ? 6 : 8, tall ? 236 : 110, "C:ENABLE & PAIR  LEFT:BACK", canvas_w);
        return;
    }

    const char *st;
    switch (wetgreg_bt_state()) {
        case BT_STARTING:    st = "STARTING..."; break;
        case BT_ADVERTISING: st = "DISCOVERABLE"; break;
        case BT_CONNECTED:   st = "CONNECTED"; break;
        case BT_PAIRED:      st = "PAIRED"; break;
        default:             st = "ON"; break;
    }

    snprintf(line, sizeof(line), "STATUS: %s", st);
    draw_text(8, y, line, canvas_w); y += dy;
    if (wetgreg_bt_peer()[0]) {
        snprintf(line, sizeof(line), "PEER: %s", wetgreg_bt_peer());
        draw_text(8, y, line, canvas_w); y += dy;
    }
    y += dy;
    if (wetgreg_bt_state() == BT_PAIRED) {
        draw_text(8, y, "PAIRED! PHONE CAN NOW", canvas_w); y += dy;
        draw_text(8, y, "READ MOOD & STEPS.", canvas_w);
    } else {
        draw_text(8, y, "FIND \"WetGreg Hub\" ON", canvas_w); y += dy;
        draw_text(8, y, "YOUR PHONE & PAIR.", canvas_w);
    }

    draw_text(tall ? 6 : 8, tall ? 236 : 110, "C:DISABLE  LEFT:BACK", canvas_w);
}
