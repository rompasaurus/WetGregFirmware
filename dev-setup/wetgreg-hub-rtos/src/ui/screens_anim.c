/**
 * screens_anim.c — full-screen animations: boot splash, intro story, sleep still.
 *
 * Purpose: implement the splash/intro/sleep-still choreography: bubbles,
 * easing, gaze/swim scripting, and per-hold layout.
 */
#include "screens_anim.h"

#include <math.h>
#include <string.h>

#include "canvas.h"
#include "motion.h"
#include "octopus.h"
#include "rng.h"
#include "text.h"

/* ─── Boot splash (STATE_SPLASH) ─────────────────────────────────────────────
 * One-shot power-on animation (Requirements/Feature_SplashBootAnimation):
 *   Phase A 0–2 s    : sassy Greg peeks in (left edge in landscape, bottom in
 *                      tall) and eases to the default position, bubbles rising.
 *   Phase B 2–3.5 s  : "WET GREG" title slides in (from the right in landscape,
 *                      from the top — with an escort bubble — in tall).
 *   Phase C    –10 s : hold — sassy idle sway, bubbles keep drifting.
 * Orientation is re-read every frame, so rotating mid-splash re-lays out. */
#define SPLASH_ENTER_MS    2000
#define SPLASH_TITLE_MS    1500   /* title slide duration, starts at SPLASH_ENTER_MS */

/* Sassy variant = side-eye pupils (CHILL) + smirk mouth. */
#define SPLASH_MOOD  MOOD_CHILL
#define SPLASH_EXPR  EXPR_SMIRK

/* Rising bubbles in 0..255 normalized coords, so an orientation change mid-
 * splash just rescales them onto the new canvas. */
#define SPLASH_NBUB 8
static struct { uint8_t x, y, r, spd; } splash_bub[SPLASH_NBUB];

void splash_bubbles_init(void) {
    for (int i = 0; i < SPLASH_NBUB; i++) {
        splash_bub[i].x   = (uint8_t)(rng_next() & 0xFF);
        splash_bub[i].y   = (uint8_t)(rng_next() & 0xFF);
        splash_bub[i].r   = (uint8_t)(2 + rng_next() % 4);     /* 2..5 px */
        splash_bub[i].spd = (uint8_t)(15 + rng_next() % 26);   /* rise px/s */
    }
}

/* Bubble = circle outline with a 1 px glint (raw canvas coords, no layout offset). */
static void draw_splash_bubble(int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= (r - 1) * (r - 1))
                px_set(cx + dx, cy + dy);
        }
    px_clr(cx - (r * 7) / 10, cy - (r * 7) / 10);
}

static void draw_splash_bubbles(uint32_t elapsed) {
    for (int i = 0; i < SPLASH_NBUB; i++) {
        int span = canvas_h + 12;                 /* wrap through a 6 px off-screen band */
        int y = (splash_bub[i].y * canvas_h) / 256 - (int)((elapsed * splash_bub[i].spd) / 1000);
        y = ((y % span) + span) % span - 6;
        int x = (splash_bub[i].x * canvas_w) / 256
              + (int)(2.5f * sinf(elapsed * 0.004f + i * 1.7f));
        draw_splash_bubble(x, y, splash_bub[i].r);
    }
}

/* Ease-out quadratic 0→1 over `dur` ms once `elapsed` passes `start`, clamped. */
static float splash_ease(uint32_t elapsed, uint32_t start, uint32_t dur) {
    if (elapsed <= start) return 0.0f;
    float p = (float)(elapsed - start) / (float)dur;
    if (p > 1.0f) p = 1.0f;
    return 1.0f - (1.0f - p) * (1.0f - p);
}

void render_splash(uint32_t elapsed, uint32_t frame_idx) {
    Quote sq; sq.text = ""; sq.mood = SPLASH_MOOD;
    float in = splash_ease(elapsed, 0, SPLASH_ENTER_MS);
    float ti = splash_ease(elapsed, SPLASH_ENTER_MS, SPLASH_TITLE_MS);
    /* Small vertical bob while sliding — rides on the layout origin so it can't
     * fight the mood body-transform inside draw_octopus(). */
    int bob = (in < 1.0f) ? (int)(2.0f * sinf(elapsed * 0.012f)) : 0;

    if (orientation_is_tall()) {
        set_canvas_tall();                       /* 122 x 250 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(elapsed);

        /* Greg rises from below the bottom edge to the tall layout's spot. */
        const int OX = (122 - 65) / 2 - 5, OY_END = 113, OY_START = 250;
        layout_ox = OX;
        layout_oy = OY_START - (int)((OY_START - OY_END) * in) + bob;
        draw_octopus(&sq, SPLASH_EXPR, frame_idx);
        layout_ox = 0; layout_oy = 0;

        /* Title drops in from the top, escorted by a bubble. */
        if (elapsed >= SPLASH_ENTER_MS) {
            int wy = -40 + (int)(70.0f * ti);    /* "WET" settles at y=30 */
            draw_text_2x((122 - 46) / 2, wy, "WET");
            draw_text_2x((122 - 62) / 2, wy + 18, "GREG");
            draw_splash_bubble(104, wy + 8, 4);
        }
    } else {
        set_canvas_wide();                       /* 250 x 122 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(elapsed);

        /* Greg peeks in from the left edge and eases to the speech position. */
        layout_ox = -70 + (int)(70.0f * in);
        layout_oy = bob;
        draw_octopus(&sq, SPLASH_EXPR, frame_idx);
        layout_ox = 0; layout_oy = 0;

        /* Title slides in from the right once Greg is in position. */
        if (elapsed >= SPLASH_ENTER_MS)
            draw_text_2x(250 - (int)((250 - 98) * ti), 40, "WET GREG");
    }
}

/* ─── Greg intro / tutorial (STATE_INTRO) ────────────────────────────────────
 * "Who is Greg" story animation (Requirements/Feature_GregIntroTutorial):
 * a sad, scared Greg drifts in from the bottom (tall) / left (wide) through
 * rising bubbles, nervously glancing around (up-right → up-left → down-right →
 * down-left), while the story shows ONE line at a time in big type in the free
 * half of the screen. After the story he keeps swimming until the hold runs
 * out; any press exits. Orientation is re-read every frame, like the splash. */
#define INTRO_ENTER_MS     2500   /* entrance drift */
#define INTRO_GAZE_T0      1500   /* nervous glance script starts */
#define INTRO_GAZE_STEP_MS 1200   /* per direction on the scripted first pass */
#define INTRO_GAZE_SLOW_MS 2600   /* per direction while idle-swimming after */
#define INTRO_TEXT_T0      2600   /* first story line begins */
#define INTRO_LINE_MS      5000   /* each line OWNS a 5 s window (one at a time) */
#define INTRO_CHAR_MS        60   /* typewriter reveal pace within the window */

/* Sad + scared: sad body/brows/mouth; the gaze script replaces the pupils. */
#define INTRO_MOOD  MOOD_SAD
#define INTRO_EXPR  EXPR_SAD

/* The story beats — shown ONE at a time in big type so each is readable on
 * the slow panel (the long first sentence is split into two beats). */
#define INTRO_NLINES 6
static const char *intro_lines[INTRO_NLINES] = {
    "Deep in the Atlantic Ocean...",
    "Swims a lone octopus...",
    "His name is Greg...",
    "He is alone...",
    "He is far from home.",
    "And he is WET...",
};

/* AC3 gaze cycle: up-right → up-left → down-right → down-left (y grows down). */
static const int8_t intro_gaze_seq[4][2] = { {2,-2}, {-2,-2}, {2,2}, {-2,2} };

/* One story line at a time: line i owns the window [T0 + i*LINE_MS, +LINE_MS),
 * typing out at the window start; the last line stays up through the hold. */
static void intro_draw_story(uint32_t elapsed, int tx, int ty, int tw) {
    if (elapsed < INTRO_TEXT_T0) return;
    uint32_t li = (elapsed - INTRO_TEXT_T0) / INTRO_LINE_MS;
    if (li >= INTRO_NLINES) li = INTRO_NLINES - 1;
    uint32_t t0 = INTRO_TEXT_T0 + li * INTRO_LINE_MS;

    char buf[40];
    size_t n = (elapsed - t0) / INTRO_CHAR_MS;
    size_t len = strlen(intro_lines[li]);
    if (n > len) n = len;
    memcpy(buf, intro_lines[li], n);
    buf[n] = '\0';
    draw_text_big(tx, ty, buf, tw);
}

void render_intro(uint32_t elapsed, uint32_t frame_idx) {
    Quote iq; iq.text = ""; iq.mood = INTRO_MOOD;
    float in = splash_ease(elapsed, 0, INTRO_ENTER_MS);

    /* Nervous gaze: wide-eyed straight ahead, then the scripted 4-glance pass,
     * then the same cycle at a slower cadence so he stays uneasy. */
    int gdx = 0, gdy = 0;
    if (elapsed >= INTRO_GAZE_T0) {
        uint32_t g = elapsed - INTRO_GAZE_T0;
        uint32_t step = g / INTRO_GAZE_STEP_MS;
        if (step >= 4)
            step = ((g - 4 * INTRO_GAZE_STEP_MS) / INTRO_GAZE_SLOW_MS) % 4;
        gdx = intro_gaze_seq[step][0];
        gdy = intro_gaze_seq[step][1];
    }

    /* Gentle swim drift. Body art covers x 6..64, y 22..92 at the layout
     * origin (Y_OFF included), so the paths below keep him in his half. */
    float sw = elapsed * 0.001f;

    /* Legs paddle the whole time — tips sweep ±4 px, ~1 stroke per second. */
    octopus_swim_set(4.0f, elapsed * 0.006f);

    if (orientation_is_tall()) {
        set_canvas_tall();                       /* 122 x 250 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(elapsed);

        /* Swim in the BOTTOM half; the entrance rises from below the edge. */
        int sx = 23 + (int)(20.0f * sinf(sw * 0.7f));
        int sy = 132 + (int)(16.0f * sinf(sw * 0.5f + 0.7f));
        layout_ox = sx;
        layout_oy = 250 - (int)((250 - sy) * in);
        octopus_gaze_set(gdx, gdy);
        draw_octopus(&iq, INTRO_EXPR, frame_idx);
        octopus_gaze_clear();
        octopus_swim_set(0.0f, 0.0f);
        layout_ox = 0; layout_oy = 0;

        /* Current story line in the TOP half, big type (wraps to ≤4 rows). */
        intro_draw_story(elapsed, 2, 24, 118);
        draw_text(6, 240, "ANY KEY: EXIT", 122);
    } else {
        set_canvas_wide();                       /* 250 x 122 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(elapsed);

        /* Swim in the LEFT half; the entrance drifts in from the left edge. */
        int sx = 24 + (int)(24.0f * sinf(sw * 0.6f));
        int sy = 8 + (int)(12.0f * sinf(sw * 0.9f + 1.3f));
        layout_ox = -80 + (int)((80 + sx) * in);
        layout_oy = (int)(sy * in);
        octopus_gaze_set(gdx, gdy);
        draw_octopus(&iq, INTRO_EXPR, frame_idx);
        octopus_gaze_clear();
        octopus_swim_set(0.0f, 0.0f);
        layout_ox = 0; layout_oy = 0;

        /* Current story line in the RIGHT half, big type (wraps to ≤4 rows). */
        intro_draw_story(elapsed, 128, 22, 118);
        draw_text(172, 113, "ANY KEY: EXIT", IMG_W);
    }
}

/* ─── Sleep screen (STATE_SLEEP) ─────────────────────────────────────────────
 * Pseudo-off still (Requirements/Feature_OffStateScreen): Greg asleep in a
 * cartoon nightcap, Z's rising to the top right, "GREG IS SLEEPING SHHHHH...."
 * as the title, "PRESS C TO WAKE HIM" at the bottom, bubbles behind like the
 * boot splash. ONE frame is rendered on entry (per orientation at that moment),
 * then the panel goes into deep sleep — e-ink holds it at zero power. */

/* A 'Z' from the 5x7 font at an integer scale (raw canvas coords). */
static void draw_sleep_z(int x0, int y0, int scale) {
    const uint8_t *g = font_glyph('Z');
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (g[row] & (0x10 >> col))
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        px_set(x0 + col * scale + dx, y0 + row * scale + dy);
}

void render_sleep_screen(void) {
    splash_bubbles_init();            /* fresh scatter each nap */
    if (orientation_is_tall()) {
        set_canvas_tall();                       /* 122 x 250 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(3000);               /* frozen mid-drift */
        clear_rows(0, 53);                       /* keep the text bands clean */
        clear_rows(234, 249);
        draw_text_big(20, 6,  "GREG IS", 122);
        draw_text_big(14, 24, "SLEEPING", 122);
        draw_text_bold(31, 44, "SHHHHH....");
        draw_sleep_z(72, 128, 1);                /* Z's rising to the top right */
        draw_sleep_z(82, 100, 2);
        draw_sleep_z(92, 64, 3);
        layout_ox = 26; layout_oy = 140;
        draw_greg_sleeping();
        layout_ox = 0; layout_oy = 0;
        draw_text(4, 238, "PRESS C TO WAKE HIM", 122);
    } else {
        set_canvas_wide();                       /* 250 x 122 */
        memset(frame, 0, sizeof(frame));
        draw_splash_bubbles(3000);
        clear_rows(0, 28);                       /* keep the text bands clean */
        clear_rows(110, 121);
        draw_text_big(30, 3, "GREG IS SLEEPING", 250);
        draw_text_bold(96, 20, "SHHHHH....");
        draw_sleep_z(100, 54, 1);                /* Z's rising to the top right */
        draw_sleep_z(132, 36, 2);
        draw_sleep_z(170, 19, 3);
        layout_ox = 30; layout_oy = 24;
        draw_greg_sleeping();
        layout_ox = 0; layout_oy = 0;
        draw_text(133, 113, "PRESS C TO WAKE HIM", 250);
    }
}
