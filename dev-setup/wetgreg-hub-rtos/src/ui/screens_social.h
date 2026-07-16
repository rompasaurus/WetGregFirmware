/**
 * screens_social.h — WetGreg-to-WetGreg social screens: the Social menu,
 * met/nearby lists, hello prompts, name picker, emote picker, and the
 * emote-playback octopus.
 *
 * Purpose: declare the social screens, the emote codes, and the social UI
 * state shared with the app handlers.
 */
#ifndef SCREENS_SOCIAL_H
#define SCREENS_SOCIAL_H

#include <stdbool.h>
#include <stdint.h>

/* ─── Emotes (WetGreg-to-WetGreg expressions) ─────────────────────────────────
 * An emote = a 1-byte code carried in the beacon. Each maps the octopus to a
 * fitting mood/face PLUS an animated overlay glyph, so the octopus stays on
 * screen and "acts out" the emote. Code 0 = none. */
#define EMOTE_NONE   0
#define EMOTE_WAVE   1
#define EMOTE_LOVE   2
#define EMOTE_LAUGH  3
#define EMOTE_PARTY  4
#define EMOTE_SLEEPY 5
#define EMOTE_WHOA   6
#define EMOTE_COUNT  7
#define EMOTE_PICK_COUNT (EMOTE_COUNT - 1)   /* skip EMOTE_NONE */

/* ─── Social menu items ─── */
#define SOCIAL_MENU_COUNT 5
#define SOC_ITEM_SCAN   0
#define SOC_ITEM_NEARBY 1
#define SOC_ITEM_MET    2
#define SOC_ITEM_NAME   3
#define SOC_ITEM_BACK   4

/* ── Social UI state (the WetGreg a prompt/recv/emote screen is currently
 * about) — owned here, driven by the app's social state handlers. ── */
extern uint16_t g_social_peer;
extern int8_t   g_social_peer_rssi;
extern uint16_t g_name_seed;        /* candidate seed on the Set-Name reroll screen */

/* Live "scan nearby" buffer — distinct WetGreg ids seen while the screen is open. */
#define NEARBY_MAX 8
extern uint16_t g_nearby_id[NEARBY_MAX];
extern int8_t   g_nearby_rssi[NEARBY_MAX];
extern int      g_nearby_count;

/* Emote playback: which emote the octopus is acting out, whether we received
 * it (vs sent), and which app state to enter once the animation ends. */
extern uint8_t  g_play_emote;
extern bool     g_play_incoming;
extern uint8_t  g_play_next;        /* a STATE_* code from app_main.c */
extern int      g_emote_sel;

void render_social_menu(int sel);

/* Scrollable list shared by "WETGREGS MET" and "SCAN NEARBY". `ids`/`rssi`
 * hold `count` entries; rssi==NULL hides the dBm column (met list). */
void render_wetgreg_list(const char *title, const uint16_t *ids, const int8_t *rssi,
                         const uint8_t *flags, int count, int sel, const char *empty,
                         const char *foot);

/* "say hi?" / "say hi back?" prompts share a layout; `incoming` flips the wording. */
void render_social_card(bool incoming);

void render_social_name(void);
void render_emote_pick(int sel);

/* The octopus acting out `emote`, with `caption`; `tick` drives the overlay. */
void render_emote_octopus(uint8_t emote, const char *caption, uint32_t tick);

#endif /* SCREENS_SOCIAL_H */
