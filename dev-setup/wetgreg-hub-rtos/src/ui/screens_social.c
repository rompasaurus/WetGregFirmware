/**
 * screens_social.c — WetGreg-to-WetGreg social screens.
 *
 * Purpose: render the social menu/lists/prompts and the emote-acting octopus
 * with its overlay glyphs.
 */
#include "screens_social.h"

#include <stdio.h>
#include <string.h>

#include "canvas.h"
#include "motion.h"
#include "octopus.h"
#include "screens_home.h"   /* draw_clock_header (wide emote layout) */
#include "storage.h"
#include "text.h"

/* ── Social UI state ── */
uint16_t g_social_peer      = 0;
int8_t   g_social_peer_rssi = 0;
uint16_t g_name_seed        = 0;

uint16_t g_nearby_id[NEARBY_MAX];
int8_t   g_nearby_rssi[NEARBY_MAX];
int      g_nearby_count = 0;

uint8_t  g_play_emote    = 0;
bool     g_play_incoming = false;
uint8_t  g_play_next     = 0;
int      g_emote_sel     = 0;

typedef struct { const char *name; uint8_t mood; uint8_t expr; } emote_def_t;
static const emote_def_t emote_defs[EMOTE_COUNT] = {
    /* NONE  */ { "-",      MOOD_NORMAL,    EXPR_SMIRK     },
    /* WAVE  */ { "WAVE",   MOOD_CHILL,     EXPR_SMILE     },
    /* LOVE  */ { "LOVE",   MOOD_EXCITED,   EXPR_EXCITED   },
    /* LAUGH */ { "LAUGH",  MOOD_SLAPHAPPY, EXPR_SLAPHAPPY },
    /* PARTY */ { "PARTY",  MOOD_EXCITED,   EXPR_EXCITED   },
    /* SLEEPY*/ { "SLEEPY", MOOD_TIRED,     EXPR_TIRED     },
    /* WHOA  */ { "WHOA",   MOOD_EXCITED,   EXPR_OPEN      },
};

/* ── small procedural glyphs (wide-canvas coords) ── */
static void draw_heart(int cx, int cy) {
    px_set(cx-2,cy); px_set(cx-1,cy); px_set(cx+1,cy); px_set(cx+2,cy);
    for (int dx=-3; dx<=3; dx++) { px_set(cx+dx,cy+1); px_set(cx+dx,cy+2); }
    for (int dx=-2; dx<=2; dx++) px_set(cx+dx,cy+3);
    px_set(cx-1,cy+4); px_set(cx,cy+4); px_set(cx+1,cy+4);
    px_set(cx,cy+5);
}
static void draw_note(int x, int y) {
    for (int a=0;a<3;a++) for (int b=0;b<3;b++) px_set(x+a, y+5+b);   /* head */
    for (int b=0;b<8;b++) px_set(x+3, y+b);                          /* stem */
    px_set(x+4,y); px_set(x+5,y+1);                                  /* flag */
}
/* A waving hand that tilts with `t` (the wave motion). */
static void draw_wave_hand(int x, int y, int t) {
    int s = (t & 1) ? 1 : -1;
    for (int a=0;a<6;a++) for (int b=0;b<5;b++) px_set(x+a+(b<2?s:0), y+b+3);  /* palm */
    for (int f=0; f<4; f++) { int fx = x + f*1 + 1; px_set(fx+s, y+1); px_set(fx+s, y+2); }
}

/* Draw the animated emote glyph relative to a base origin (bx,by) so the same
 * code serves both orientations — only the base differs. */
static void draw_emote_overlay(uint8_t emote, int bx, int by, uint32_t tick) {
    switch (emote) {
        case EMOTE_WAVE:  draw_wave_hand(bx + 2, by + 4, (int)tick); break;
        case EMOTE_LOVE:
            for (int i = 0; i < 3; i++) draw_heart(bx + 6 + i*13, by + 32 - (int)((tick*3 + i*7) % 32));
            break;
        case EMOTE_LAUGH: draw_text(bx, by + 2, (tick & 1) ? "HA HA" : " HAHA", IMG_W); break;
        case EMOTE_PARTY:
            draw_note(bx + 2 + ((tick&1)?0:3), by + 2);
            draw_note(bx + 24, by + 10 + ((tick&1)?2:0));
            draw_note(bx + 44, by + ((tick&1)?0:3));
            break;
        case EMOTE_SLEEPY: {
            int n = (int)(tick % 3) + 1;
            for (int i = 0; i < n; i++) draw_text(bx + 6 + i*12, by + 18 - i*9, "Z", IMG_W);
            break;
        }
        case EMOTE_WHOA:  draw_text(bx + 2, by + 2, (tick & 1) ? "! !" : "!!!", IMG_W); break;
        default: break;
    }
}

/* Octopus acting out an emote — works in BOTH orientations (wide: octopus left,
 * caption right; tall: caption top, octopus at the bottom like the main screen). */
void render_emote_octopus(uint8_t emote, const char *caption, uint32_t tick) {
    if (emote >= EMOTE_COUNT) emote = EMOTE_WAVE;
    const emote_def_t *e = &emote_defs[emote];
    Quote tq; tq.text = ""; tq.mood = e->mood;
    char nm[20]; snprintf(nm, sizeof(nm), "* %s *", e->name);

    if (orientation_is_tall()) {
        set_canvas_tall();
        memset(frame, 0, sizeof(frame));
        draw_text(6, 12, caption, 122);
        draw_text(6, 30, nm, 122);
        for (int x = 4; x < 118; x++) px_set(x, 44);
        layout_ox = (122 - 65) / 2 - 5; layout_oy = 113;   /* same spot as the main tall octopus */
        draw_octopus(&tq, e->expr, tick);
        draw_emote_overlay(emote, 36, 70, tick);            /* just above the head */
    } else {
        set_canvas_wide();
        memset(frame, 0, sizeof(frame));
        layout_ox = 0; layout_oy = 0;
        draw_clock_header();
        draw_octopus(&tq, e->expr, tick);
        draw_emote_overlay(emote, 58, 12, tick);
        draw_text(140, 34, caption, 108);
        draw_text(140, 52, nm, 108);
    }
}

/* ─── Social screens (WetGreg-to-WetGreg) ─── */
void render_social_menu(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 40, tall ? 10 : 3, "SOCIAL", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);

    char line[40];
    int y = tall ? 28 : 17, dy = tall ? 17 : 12;
    snprintf(line, sizeof(line), "ME: %s", wetgreg_display_name());
    draw_text(8, y, line, canvas_w); y += dy;
    snprintf(line, sizeof(line), "ID #%04X  MET %lu",
             g_saved.wetgreg_id, (unsigned long)g_saved.met_count);
    draw_text(8, y, line, canvas_w); y += dy + 2;

    const char *items[SOCIAL_MENU_COUNT];
    static char it0[24];
    snprintf(it0, sizeof(it0), "SCAN: %s", g_saved.social_on ? "ON" : "OFF");
    items[SOC_ITEM_SCAN]   = it0;
    items[SOC_ITEM_NEARBY] = "SCAN NEARBY";
    items[SOC_ITEM_MET]    = "WETGREGS MET";
    items[SOC_ITEM_NAME]   = "SET NAME";
    items[SOC_ITEM_BACK]   = "BACK";
    for (int i = 0; i < SOCIAL_MENU_COUNT; i++) {
        int yy = y + i * dy; char l[28];
        if (i == sel) { snprintf(l, sizeof(l), "> %s", items[i]); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", items[i]); draw_text(8, yy, l, canvas_w); }
    }
    draw_text(4, tall ? 232 : 108, "U/D  C:SEL  L:BACK", canvas_w);
}

/* Scrollable list helper shared by "WETGREGS MET" and "SCAN NEARBY". `ids`/`rssi`
 * hold `count` entries; rssi==NULL hides the dBm column (met list). */
void render_wetgreg_list(const char *title, const uint16_t *ids, const int8_t *rssi,
                         const uint8_t *flags, int count, int sel, const char *empty,
                         const char *foot) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 24, tall ? 10 : 3, title, canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);
    if (count <= 0) {
        draw_text(8, tall ? 60 : 40, empty, canvas_w);
        draw_text(4, tall ? 232 : 108, "L:BACK", canvas_w);
        return;
    }
    int rows = tall ? 9 : 6, top = (sel >= rows) ? sel - rows + 1 : 0;
    int y0 = tall ? 30 : 20, dy = tall ? 20 : 13;
    for (int i = 0; i < rows && (top + i) < count; i++) {
        int idx = top + i;
        char nm[24]; wetgreg_auto_name(ids[idx], nm, sizeof(nm));
        char line[40];
        if (rssi) {
            snprintf(line, sizeof(line), "%s %ddBm", nm, (int)rssi[idx]);
        } else {
            char mk[4]; int k = 0;
            if (flags && (flags[idx] & MET_HELLO_SENT)) mk[k++] = '>';
            if (flags && (flags[idx] & MET_HELLO_RECV)) mk[k++] = '<';
            mk[k] = '\0';
            snprintf(line, sizeof(line), "%s %s", nm, mk);
        }
        int y = y0 + i * dy;
        if (idx == sel) { char l[44]; snprintf(l, sizeof(l), "> %s", line); draw_inverted_line(y, l); }
        else            { draw_text(8, y, line, canvas_w); }
    }
    draw_text(4, tall ? 232 : 108, foot, canvas_w);
}

/* "say hi?" / "say hi back?" prompts share a layout; `incoming` flips the wording. */
void render_social_card(bool incoming) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    char nm[24]; wetgreg_auto_name(g_social_peer, nm, sizeof(nm));
    char line[40];
    int y = tall ? 24 : 6, dy = tall ? 18 : 12;
    if (incoming) {
        draw_text(8, y, "ANOTHER WETGREG", canvas_w); y += dy;
        draw_text(8, y, "SAYS HELLO!", canvas_w); y += dy + 3;
    } else {
        draw_text(8, y, "A WETGREG APPEARS", canvas_w); y += dy;
        draw_text(8, y, "IN THE WILD!", canvas_w); y += dy + 3;
    }
    draw_text(8, y, nm, canvas_w); y += dy;
    snprintf(line, sizeof(line), "#%04X  %d dBm", g_social_peer, (int)g_social_peer_rssi);
    draw_text(8, y, line, canvas_w); y += dy + 3;
    draw_text(8, y, incoming ? "RESPOND?" : "SAY HI?", canvas_w);
    draw_text(4, tall ? 232 : 108, incoming ? "C:EMOTE  L:NO" : "C:YES  L:NO", canvas_w);
}

void render_social_name(void) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    char nm[24]; wetgreg_auto_name(g_name_seed, nm, sizeof(nm));
    int y = tall ? 24 : 8, dy = tall ? 22 : 18;
    draw_text(8, y, "PICK A NAME:", canvas_w); y += dy + 4;
    draw_text(8, y, nm, canvas_w);
    draw_text(4, tall ? 232 : 108, "U/D:REROLL C:SAVE L:NO", canvas_w);
}

void render_emote_pick(int sel) {
    bool tall = orientation_is_tall();
    if (tall) set_canvas_tall(); else set_canvas_wide();
    memset(frame, 0, sizeof(frame));
    draw_text(tall ? 8 : 24, tall ? 10 : 3, "SEND EMOTE", canvas_w);
    for (int x = 4; x < canvas_w - 4; x++) px_set(x, tall ? 22 : 14);
    char nm[24]; wetgreg_auto_name(g_social_peer, nm, sizeof(nm));
    char line[40]; snprintf(line, sizeof(line), "TO: %s", nm);
    int y = tall ? 28 : 18, dy = tall ? 17 : 13;
    draw_text(8, y, line, canvas_w); y += dy + 2;
    for (int i = 0; i < EMOTE_PICK_COUNT; i++) {
        int yy = y + i * dy; char l[28];
        const char *n = emote_defs[i + 1].name;
        if (i == sel) { snprintf(l, sizeof(l), "> %s", n); draw_inverted_line(yy, l); }
        else          { snprintf(l, sizeof(l), "  %s", n); draw_text(8, yy, l, canvas_w); }
    }
    draw_text(4, tall ? 232 : 108, "U/D C:SEND L:BACK", canvas_w);
}
