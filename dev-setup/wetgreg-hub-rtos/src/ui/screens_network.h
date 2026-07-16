/**
 * screens_network.h — connectivity screens: WiFi status, Network submenu,
 * scan results, the on-screen password keyboard, saved networks, and the
 * Bluetooth pairing screen.
 *
 * Purpose: declare the connectivity screens and the keyboard/selection state
 * they share with the app handlers.
 *
 * Renderers only — input handling stays in the app state machine. The
 * keyboard/selection state below is shared with those handlers.
 */
#ifndef SCREENS_NETWORK_H
#define SCREENS_NETWORK_H

#include <stdbool.h>

/* ─── WiFi status (read-only) ─── */
void render_network_screen(void);

/* ─── Network submenu ─── */
#define NET_ITEM_ONOFF      0
#define NET_ITEM_SCAN       1
#define NET_ITEM_SAVED      2
#define NET_ITEM_STATUS     3
#define NET_ITEM_BACK       4
#define NET_MENU_COUNT      5
void render_net_menu(int sel);

/* ─── Scan results ─── */
void render_scan_results(void);

/* ─── On-screen keyboard (WiFi password entry) ─── */
#define PW_MAX_LEN 63
#define KB_CHAR_ROWS 4
#define KB_SPECIAL_ROW 4
#define KB_SP_SHIFT  0
#define KB_SP_SPACE  1
#define KB_SP_DEL    2
#define KB_SP_DONE   3
#define KB_SP_CANCEL 4
#define KB_SP_COUNT  5

/* Keyboard state — owned here, driven by the app's STATE_NET_KEYBOARD
 * handler (cursor moves, character inserts). */
extern char pw_buf[PW_MAX_LEN + 1];
extern int  pw_len;
extern int  kb_row;
extern int  kb_col;
extern bool kb_shift;
extern int  selected_network;   /* index into scan_results, -1 = none */

char kb_char_at(int row, int col, bool shift);
void render_keyboard(void);

/* ─── Saved networks (connect / forget) ─── */
extern int saved_sel;
void render_saved_nets(void);

/* "CONNECTING TO ..." interstitial (pushed before the blocking join). */
void show_connecting_screen(const char *ssid);

/* ─── Bluetooth pairing screen ───
 * `enabled` = the app's user-facing BT toggle (radio allowed on). */
void render_bluetooth(bool enabled);

#endif /* SCREENS_NETWORK_H */
