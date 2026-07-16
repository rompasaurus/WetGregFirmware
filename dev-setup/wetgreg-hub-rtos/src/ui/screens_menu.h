/**
 * screens_menu.h — the menu system: main menu, generic tall list/text
 * renderers, and the Settings-family screens (mood, display, animations,
 * sound, info, set-time, reset, motion, I2C scan).
 *
 * Purpose: declare the menu-system renderers and the item-index codes the
 * app's input handlers dispatch on.
 *
 * Renderers only — input handling stays in the app state machine, which
 * passes the selection index in and reads the *_ITEM_* codes out.
 */
#ifndef SCREENS_MENU_H
#define SCREENS_MENU_H

#include <stdbool.h>

#include "rtc_compat.h"   /* datetime_t (the set-time editor state) */

/* ─── Main menu ─── */
#define MENU_COUNT 8
#define MENU_IDX_MOOD      0
#define MENU_IDX_ANIM      1
#define MENU_IDX_SOUND     2
#define MENU_IDX_MOTION    3
#define MENU_IDX_INFO      4
#define MENU_IDX_SOCIAL    5
#define MENU_IDX_SETTINGS  6
#define MENU_IDX_BACK      7

void render_menu(int selected);        /* wide: overlay on the octopus frame */
void render_menu_tall(int selected);   /* tall: full-screen list */

/* ─── Generic TALL (122x250) renderers, so every menu/list is readable and
 *     navigable when the device is held joystick-at-bottom. ─── */
void render_list_tall(const char *title, const char *const *items,
                      int count, int sel, const char *footer);
void render_text_tall(const char *title, const char *const *lines, int n);

/* ─── Mood select ─── */
void render_mood_select(int selected);

/* ─── Settings submenu ─── */
#define SET_MENU_COUNT 6
#define SET_ITEM_NETWORK   0
#define SET_ITEM_BLUETOOTH 1
#define SET_ITEM_DISPLAY   2
#define SET_ITEM_SET_TIME  3
#define SET_ITEM_RESET     4
#define SET_ITEM_BACK      5
void render_settings_menu(int sel);

/* ─── Display settings (auto-rotate + orientation) ─── */
#define DISP_MENU_COUNT 3
#define DISP_ITEM_AUTO   0
#define DISP_ITEM_ORIENT 1
#define DISP_ITEM_BACK   2
void render_display_menu(int sel);

/* ─── Animations submenu ─── */
#define ANIM_MENU_COUNT 2
#define ANIM_ITEM_INTRO 0
#define ANIM_ITEM_BACK  1
void render_anim_menu(int sel);

/* ─── Sound submenu ─── */
#define SND_ITEM_PATTERN 0
#define SND_ITEM_ONOFF   1
#define SND_ITEM_VOL     2
#define SND_ITEM_BACK    3
#define SND_MENU_COUNT   4
void render_sound_menu(int sel);

/* ─── Device info ─── */
void render_info_screen(void);

/* ─── Manual date/time setter ───
 * The editor state is shared with the app's STATE_SET_TIME handler. */
extern datetime_t settime_dt;
extern int  settime_field;     /* 0=year 1=month 2=day 3=hour 4=min */
extern bool settime_editing;   /* false = pick a field, true = adjust its value */
void render_set_time(void);

/* ─── Factory reset confirmation ─── */
void render_reset_confirm(void);

/* ─── Motion / accelerometer menu ─── */
#define MOT_ITEM_LIVE      0
#define MOT_ITEM_PEDOMETER 1
#define MOT_ITEM_TILT      2
#define MOT_ITEM_RESET     3
#define MOT_ITEM_THRESH    4
#define MOT_ITEM_I2CSCAN   5
#define MOT_ITEM_BACK      6
#define MOT_MENU_COUNT     7
void render_motion_menu(int sel);

/* I2C bus scan debug screen — scans the bus inline and renders the results
 * (pushes an intermediate "SCANNING..." frame itself). */
void render_i2c_scan(void);

#endif /* SCREENS_MENU_H */
