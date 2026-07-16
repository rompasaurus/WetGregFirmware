/**
 * octopus.c — Greg the octopus: procedural character art.
 *
 * Purpose: implement all of Greg's procedural art and per-mood animation
 * transforms; every pupil/mouth/brow helper stays private.
 *
 * Everything here draws in body-local coordinates through px_set_off/
 * px_clr_off, which add the layout origin + Y_OFF + the per-frame body
 * transform (shift / span expansion / row wobble / swim kick).
 */
#include "octopus.h"

#include <math.h>
#include <stdbool.h>

#include "canvas.h"

const char *const mood_names[MOOD_COUNT] = {
    "NORMAL", "WEIRD", "UNHINGED", "ANGRY",
    "SAD", "CHAOTIC", "HUNGRY", "TIRED",
    "SLAP HAPPY", "LAZY", "FAT", "CHILL",
    "CREEPY", "EXCITED", "NOSTALGIC", "HOMESICK",
    "WISE",
};

int current_mood = -1;  /* -1 = all moods (random) */

/* Layout origin — repositions the whole octopus (e.g. tall layout, splash). */
int layout_ox = 0, layout_oy = 0;

/* ─── Body animation transform (set per-frame before rendering) ─── */
static int body_dx = 0;       /* global x shift */
static int body_dy = 0;       /* global y shift */
static int body_x_expand = 0; /* expand/shrink body spans (+ = wider) */
/* Per-row wobble amplitude and phase (for wavy effects) */
static float wobble_amp = 0;
static float wobble_freq = 0;
static float wobble_phase = 0;

/* Tentacle swim-kick (intro): a traveling wave down the tentacle rows of the
 * body art (y >= 55), anchored at the body and growing toward the tips, so the
 * legs undulate like a swimming octopus. Set per-frame by the intro renderer,
 * zero everywhere else. */
static float g_swim_amp = 0;
static float g_swim_phase = 0;

/* Scriptable gaze (intro animation): when set, draw_octopus() uses this
 * instead of the mood's pupils. */
static bool g_gaze_override = false;
static int  g_gaze_dx = 0, g_gaze_dy = 0;

void octopus_gaze_set(int dx, int dy) {
    g_gaze_override = true;
    g_gaze_dx = dx;
    g_gaze_dy = dy;
}

void octopus_gaze_clear(void) { g_gaze_override = false; }

void octopus_swim_set(float amp, float phase) {
    g_swim_amp = amp;
    g_swim_phase = phase;
}

static int row_wobble(int y) {
    int w = 0;
    if (wobble_amp != 0)
        w = (int)(wobble_amp * sinf(y * wobble_freq + wobble_phase));
    if (g_swim_amp != 0 && y >= 55) {
        float depth = (float)(y - 54) / 26.0f;   /* 0 at the body, 1 at the tips */
        w += (int)(g_swim_amp * depth * sinf(g_swim_phase - (y - 54) * 0.4f));
    }
    return w;
}

/* Offset pixel helpers — add layout origin + Y_OFF + body transform. */
static inline void px_set_off(int x, int y) {
    px_set(x + body_dx + row_wobble(y) + layout_ox, y + Y_OFF + body_dy + layout_oy);
}
static inline void px_clr_off(int x, int y) {
    px_clr(x + body_dx + row_wobble(y) + layout_ox, y + Y_OFF + body_dy + layout_oy);
}

/* ─── Octopus body (RLE: y, num_spans, x0, x1, ...) terminated by 0xFF ─── */
static const uint8_t body_rle[] = {
    10,1, 22,48,  11,1, 18,52,  12,1, 16,54,  13,1, 14,56,
    14,1, 13,57,  15,1, 12,58,  16,1, 11,59,  17,1, 10,60,
    18,1, 10,60,  19,1,  9,61,  20,1,  9,61,  21,1,  9,61,
    22,1,  9,61,  23,1,  9,61,  24,1,  9,61,  25,1,  9,61,
    26,1,  9,61,  27,1,  9,61,  28,1, 10,60,  29,1, 10,60,
    30,1, 10,60,  31,1, 10,60,  32,1, 10,60,  33,1, 10,60,
    34,1, 10,60,  35,1, 10,60,  36,1, 10,60,  37,1, 10,60,
    38,1, 10,60,  39,1, 10,60,  40,1, 10,60,  41,1, 11,59,
    42,1, 11,59,  43,1, 12,58,  44,1, 13,57,  45,1, 14,56,
    46,1, 12,58,  47,1, 11,59,  48,1, 10,60,  49,1, 10,60,
    50,1, 11,59,  51,1, 12,58,  52,1, 13,57,  53,1, 14,56,
    54,1, 15,55,
    /* Tentacles */
    55,5, 10,17, 21,28, 32,39, 43,50, 54,61,
    56,5,  8,15, 19,26, 30,37, 45,52, 56,63,
    57,5,  7,14, 18,24, 29,35, 47,53, 58,64,
    58,5,  6,12, 19,25, 31,37, 46,52, 57,63,
    59,5,  7,13, 21,27, 33,39, 44,50, 55,61,
    60,5,  8,14, 20,26, 31,37, 43,49, 54,60,
    61,5,  9,14, 18,24, 30,36, 44,50, 56,62,
    62,5,  8,13, 17,22, 31,37, 46,52, 57,63,
    63,5,  7,12, 18,23, 33,38, 45,51, 55,61,
    64,5,  8,13, 20,25, 32,37, 43,48, 54,59,
    65,5,  9,14, 19,24, 30,35, 44,49, 55,60,
    66,5, 10,14, 17,22, 31,36, 46,51, 57,62,
    67,5,  9,13, 18,22, 33,37, 45,50, 56,61,
    68,5,  8,12, 19,23, 32,36, 43,48, 54,59,
    69,5,  9,13, 21,25, 30,34, 44,48, 55,59,
    70,5, 10,14, 20,24, 31,35, 46,50, 57,61,
    71,5, 11,14, 18,22, 33,37, 45,49, 56,60,
    72,5, 10,13, 19,22, 32,35, 43,47, 54,58,
    73,5,  9,12, 20,23, 30,33, 44,47, 55,58,
    74,5, 10,13, 21,24, 31,34, 46,49, 57,60,
    75,5, 11,14, 20,23, 33,36, 45,48, 56,59,
    76,5, 12,14, 19,22, 32,35, 43,46, 54,57,
    77,5, 11,13, 20,22, 30,33, 44,46, 55,57,
    78,5, 10,12, 21,23, 31,33, 45,47, 56,58,
    79,5, 11,13, 22,24, 32,34, 44,46, 55,57,
    80,5, 12,14, 21,23, 33,35, 43,45, 54,56,
    0xFF /* terminator */
};

/* ─── Drawing primitives ─── */

static void fill_circle(int cx, int cy, int r_sq, int set) {
    int r = 5;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r_sq) {
                if (set) px_set_off(cx + dx, cy + dy);
                else     px_clr_off(cx + dx, cy + dy);
            }
}

/* Body with span expansion applied (uses body_x_expand global) */
static void draw_body_transformed(void) {
    const uint8_t *p = body_rle;
    while (*p != 0xFF) {
        int y = *p++;
        int n = *p++;
        for (int i = 0; i < n; i++) {
            int x0 = *p++;
            int x1 = *p++;
            int ax0 = x0 - body_x_expand;
            int ax1 = x1 + body_x_expand;
            if (ax0 < 0) ax0 = 0;
            if (ax1 >= IMG_W) ax1 = IMG_W - 1;
            for (int x = ax0; x <= ax1; x++)
                px_set_off(x, y);
        }
    }
}

static void draw_eyes(void) {
    /* White eye sockets: two circles r²=16 at (22,25) and (48,25) */
    fill_circle(22, 25, 16, 0);
    fill_circle(48, 25, 16, 0);
}

static void draw_pupils_normal(void) {
    /* Black pupils: r²=4 at (23,26) and (49,26) */
    fill_circle(23, 26, 4, 1);
    fill_circle(49, 26, 4, 1);
    /* White highlights: r²=1 at (20,23) and (46,23) */
    fill_circle(20, 23, 1, 0);
    fill_circle(46, 23, 1, 0);
}

/* Normal pupils offset by (dx,dy) inside the sockets (the scripted gaze). */
static void draw_pupils_gaze(int dx, int dy) {
    fill_circle(23 + dx, 26 + dy, 4, 1);
    fill_circle(49 + dx, 26 + dy, 4, 1);
    fill_circle(20 + dx, 23 + dy, 1, 0);
    fill_circle(46 + dx, 23 + dy, 1, 0);
}

static void draw_pupils_weird(void) {
    /* Misaligned: left up-left (21,24), right down-right (50,28) */
    fill_circle(21, 24, 4, 1);
    fill_circle(50, 28, 4, 1);
    fill_circle(20, 23, 1, 0);
    fill_circle(46, 23, 1, 0);
}

static void draw_pupils_unhinged(void) {
    /* Tiny pinprick pupils, no highlights */
    px_set_off(22, 25); px_set_off(23, 25); px_set_off(22, 26); px_set_off(23, 26);
    px_set_off(48, 25); px_set_off(49, 25); px_set_off(48, 26); px_set_off(49, 26);
}

static void draw_pupils_angry(void) {
    /* Pupils shifted inward and down — glaring toward the nose */
    fill_circle(25, 27, 4, 1);  /* left: shifted right+down */
    fill_circle(47, 27, 4, 1);  /* right: shifted left+down */
    fill_circle(23, 24, 1, 0);  /* highlights */
    fill_circle(45, 24, 1, 0);
}

static void draw_brows_angry(void) {
    /* Thick slanted half-circle arcs across top of eye sockets.
       Outer edges high, inner edges low — angry V shape.
       Must match Python _octo_angry_eyes(). */
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 14 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(20 + t * 5 - arc);
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
        px_set_off(x + 1, y + 1);
    }
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 40 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(25 - t * 5 - arc);
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
        px_set_off(x + 1, y + 1);
    }
}

static void draw_pupils_sad(void) {
    /* Pupils shifted downward — looking at the floor */
    fill_circle(23, 28, 4, 1);
    fill_circle(49, 28, 4, 1);
    fill_circle(21, 25, 1, 0);
    fill_circle(47, 25, 1, 0);
}

static void draw_brows_sad(void) {
    /* Droopy brows — outer edges low, inner edges high (inverse of angry) */
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 14 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(25 - t * 5 - arc);  /* inner high, outer low */
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
    }
    for (int i = 0; i < 18; i++) {
        float t = i / 17.0f;
        int x = 40 + (int)(t * 16);
        float arc = 2.5f * sinf(t * 3.14159f);
        int y = (int)(20 + t * 5 - arc);
        for (int dy = 0; dy < 3; dy++) px_set_off(x, y + dy);
    }
}

static void draw_pupils_chaotic(void) {
    /* Spiral/ring eyes — concentric circles for dizzy look */
    for (int ecx_i = 0; ecx_i < 2; ecx_i++) {
        int ecx = (ecx_i == 0) ? 22 : 48;
        /* Outer ring */
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int dist = dx * dx + dy * dy;
                if (dist >= 5 && dist <= 9)
                    px_set_off(ecx + dx, 25 + dy);
            }
        /* Center dot */
        px_set_off(ecx, 25);
    }
}

/* ─── Mouth expressions ─── */

static void draw_mouth_smirk(void) {
    for (int x = 28; x < 44; x++) {
        float t = (x - 28) / 15.0f;
        float tilt = -2.0f + t * 4.0f;
        float v = 2.0f * t - 1.0f;
        float arc = (fabsf(v) < 1.0f) ? 5.0f * sqrtf(1.0f - v * v) : 0.0f;
        int yc = (int)(39.0f + tilt + arc);
        px_clr_off(x, yc);
        px_set_off(x, yc - 1);
        px_set_off(x, yc + 1);
    }
}

static void draw_mouth_smile(void) {
    for (int x = 26; x < 45; x++) {
        int cy = 38 + ((x - 35) * (x - 35)) / 25;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

static void draw_mouth_open(void) {
    int cx = 35, cy = 40, rx = 7, ry = 5;
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -6; dx <= 6; dx++)
            if (dx*dx*16 + dy*dy*36 <= 36*16)
                px_clr_off(cx + dx, cy + dy);
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx + dx, cy + dy);
                    break;
                }
            }
        }
}

static void draw_mouth_weird(void) {
    for (int x = 24; x < 48; x++) {
        float t = (x - 24) / 23.0f;
        int yc = 39 + (int)(3.5f * sinf(t * 3.14159f * 3.0f));
        px_clr_off(x, yc);
        px_set_off(x, yc - 1);
        px_set_off(x, yc + 1);
    }
}

static void draw_mouth_unhinged(void) {
    int cx = 35, cy = 41, rx = 10, ry = 7;
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -9; dx <= 9; dx++)
            if (dx*dx*36 + dy*dy*81 <= 81*36)
                px_clr_off(cx + dx, cy + dy);
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx + dx, cy + dy);
                    break;
                }
            }
        }
    for (int x = cx - 7; x <= cx + 7; x += 3) {
        px_set_off(x, cy - 5);
        px_set_off(x, cy - 4);
        px_set_off(x + 1, cy - 4);
    }
}

static void draw_mouth_angry(void) {
    /* Tight downward frown — inverted parabola */
    for (int x = 28; x < 43; x++) {
        int cy = 40 - ((x - 35) * (x - 35)) / 20;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

static void draw_mouth_sad(void) {
    /* Gentle downward curve frown */
    for (int x = 26; x < 45; x++) {
        int cy = 42 - ((x - 35) * (x - 35)) / 30;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

static void draw_mouth_chaotic(void) {
    /* Zigzag lightning-bolt mouth */
    for (int x = 24; x < 48; x++) {
        int phase = (x - 24) % 6;
        int y = (phase < 3) ? 38 + phase * 2 : 44 - phase * 2 + 6;
        px_set_off(x, y);
        px_set_off(x, y + 1);
    }
}

static void draw_pupils_hungry(void) {
    /* Pupils shifted upward — staring at imaginary food */
    fill_circle(23, 23, 4, 1);
    fill_circle(49, 23, 4, 1);
    fill_circle(21, 21, 1, 0);
    fill_circle(47, 21, 1, 0);
}

static void draw_mouth_hungry(void) {
    /* Drooling open mouth — wide oval + drool drops */
    int cx = 35, cy = 40, rx = 8, ry = 5;
    /* White interior */
    for (int dy = -(ry-1); dy <= ry-1; dy++)
        for (int dx = -(rx-1); dx <= rx-1; dx++)
            if (dx*dx*(ry-1)*(ry-1) + dy*dy*(rx-1)*(rx-1) <= (rx-1)*(rx-1)*(ry-1)*(ry-1))
                px_clr_off(cx+dx, cy+dy);
    /* Black border */
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx+dx, cy+dy);
                    break;
                }
            }
        }
    /* Drool drops */
    for (int dy = 1; dy < 6; dy++) px_set_off(33, cy+ry+dy);
    for (int dy = 1; dy < 4; dy++) px_set_off(37, cy+ry+dy+1);
}

static void draw_pupils_tired(void) {
    /* Tiny sleepy pupils low in half-closed eyes */
    for (int dx = -1; dx <= 1; dx++) {
        px_set_off(22+dx, 27); px_set_off(22+dx, 28);
        px_set_off(48+dx, 27); px_set_off(48+dx, 28);
    }
}

static void draw_lids_tired(void) {
    /* Half-closed eyelids: fill top half of eye sockets black */
    for (int ecx_i = 0; ecx_i < 2; ecx_i++) {
        int ecx = (ecx_i == 0) ? 22 : 48;
        for (int dy = -4; dy < -1; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx*dx + dy*dy <= 16)
                    px_set_off(ecx+dx, 25+dy);
    }
}

static void draw_mouth_tired(void) {
    /* Yawn mouth — tall oval, open wide vertically */
    int cx = 35, cy = 40, rx = 5, ry = 7;
    for (int dy = -(ry-1); dy <= ry-1; dy++)
        for (int dx = -(rx-1); dx <= rx-1; dx++)
            if (dx*dx*(ry-1)*(ry-1) + dy*dy*(rx-1)*(rx-1) <= (rx-1)*(rx-1)*(ry-1)*(ry-1))
                px_clr_off(cx+dx, cy+dy);
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx*dx*ry*ry + dy*dy*rx*rx > rx*rx*ry*ry) continue;
            for (int nd = 0; nd < 4; nd++) {
                int nx = dx + ((nd==0)?-1:(nd==1)?1:0);
                int ny = dy + ((nd==2)?-1:(nd==3)?1:0);
                if (nx*nx*ry*ry + ny*ny*rx*rx > rx*rx*ry*ry) {
                    px_set_off(cx+dx, cy+dy);
                    break;
                }
            }
        }
}

static void draw_eyes_slaphappy(void) {
    /* Left eye: squint shut (fill back to black, white slit) */
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
            if (dx*dx + dy*dy <= 16)
                px_set_off(22+dx, 25+dy);
    for (int dx = -3; dx <= 3; dx++)
        px_clr_off(22+dx, 25);
    /* Right eye: oversized pupil */
    fill_circle(49, 26, 9, 1);
}

static void draw_mouth_slaphappy(void) {
    /* Wide wobbly grin */
    for (int x = 22; x < 49; x++) {
        float t = (x - 22) / 26.0f;
        int base = 38 + ((x-35)*(x-35)) / 20;
        int wobble = (int)(1.5f * sinf(t * 3.14159f * 4.0f));
        int y = base + wobble;
        px_set_off(x, y);
        px_set_off(x, y+1);
    }
}

/* ─── Lazy: nearly-closed eyes, flat mouth ─── */

static void draw_lids_lazy(void) {
    /* Cover most of each eye socket — leave only bottom sliver open */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        for (int dy = -4; dy < 2; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx*dx + dy*dy <= 16)
                    px_set_off(ecx+dx, 25+dy);
    }
}

static void draw_pupils_lazy(void) {
    /* Barely visible dots low in the slit */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        px_set_off(ecx, 28);
        px_set_off(ecx+1, 28);
    }
}

static void draw_mouth_lazy(void) {
    /* Flat horizontal line — minimal effort */
    for (int x = 29; x < 42; x++) {
        px_set_off(x, 40);
        px_set_off(x, 41);
    }
}

/* ─── Fat: content wide pupils, smile with cheek puffs ─── */

static void draw_pupils_fat(void) {
    /* Wider pupils — happy and satisfied */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 49 : 23;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx*dx + dy*dy <= 9)
                    px_set_off(ecx+dx, 26+dy);
    }
}

static void draw_mouth_fat(void) {
    /* Wide satisfied smile + cheek puffs */
    for (int x = 24; x < 47; x++) {
        int cy = 38 + ((x-35)*(x-35)) / 18;
        px_set_off(x, cy);
        px_set_off(x, cy+1);
    }
    /* Cheek puffs */
    int cheeks[][2] = {{23,39},{47,39}};
    for (int c = 0; c < 2; c++)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(cheeks[c][0]+dx, cheeks[c][1]+dy);
}

/* ─── Chill: side-glancing pupils, relaxed half-smile ─── */

static void draw_pupils_chill(void) {
    /* Pupils shifted right — looking to the side */
    int centers[][2] = {{25,26},{51,26}};
    for (int e = 0; e < 2; e++)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(centers[e][0]+dx, centers[e][1]+dy);
}

static void draw_mouth_chill(void) {
    /* Slight asymmetric half-smile — relaxed */
    for (int x = 29; x < 44; x++) {
        float t = (x - 29) / 14.0f;
        int y = 40 + (int)(1.5f * t * t);
        px_set_off(x, y);
        px_set_off(x, y+1);
    }
}

/* ─── Wise: silly crossed pupils behind round spectacles, goofy tooth grin ─── */
static void draw_pupils_wise(void) {
    /* Slightly crossed and uneven — big brain, zero focus */
    fill_circle(25, 26, 4, 1);
    fill_circle(46, 27, 4, 1);
    fill_circle(23, 24, 1, 0);
    fill_circle(44, 25, 1, 0);
}

static void draw_glasses_wise(void) {
    /* Round spectacle rims as white line-art (radius ~7-8.5 around each
     * socket) plus a bridge and temple arms — sits clear of the pupils, so it
     * can draw after them like the brows do. */
    const int cxs[2] = { 22, 48 }, cy = 25;
    for (int e = 0; e < 2; e++)
        for (int dy = -9; dy <= 9; dy++)
            for (int dx = -9; dx <= 9; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 > 49 && d2 <= 72) px_clr_off(cxs[e] + dx, cy + dy);
            }
    for (int x = 30; x <= 40; x++) { px_clr_off(x, 23); px_clr_off(x, 24); }  /* bridge */
    for (int x = 10; x <= 13; x++) { px_clr_off(x, 23); px_clr_off(x, 24); }  /* left arm */
    for (int x = 57; x <= 60; x++) { px_clr_off(x, 23); px_clr_off(x, 24); }  /* right arm */
}

static void draw_mouth_wise(void) {
    /* Goofy open grin: white lower half-ellipse with tooth separators */
    const int cx = 36, cy = 39, rx = 9, ry = 6;
    for (int dy = 0; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++)
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                px_clr_off(cx + dx, cy + dy);
    for (int dy = 0; dy <= 2; dy++) {
        px_set_off(cx - 4, cy + dy);
        px_set_off(cx,     cy + dy);
        px_set_off(cx + 4, cy + dy);
    }
}

/* ─── Creepy: heart-shaped pupils, tongue-out mouth ─── */

static void draw_pupils_creepy(void) {
    /* Heart-shaped pupils in each eye socket */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        /* Top bumps */
        static const int8_t top[][2] = {{-2,-1},{-1,-2},{0,-1},{1,-2},{2,-1}};
        for (int i = 0; i < 5; i++)
            px_set_off(ecx+top[i][0], 25+top[i][1]);
        /* Middle row */
        for (int dx = -2; dx <= 2; dx++)
            px_set_off(ecx+dx, 25);
        /* Lower taper */
        for (int dx = -1; dx <= 1; dx++)
            px_set_off(ecx+dx, 26);
        /* Bottom point */
        px_set_off(ecx, 27);
    }
}

static void draw_mouth_creepy(void) {
    /* Wide open smile with tongue hanging out */
    int cx = 35, cy = 39, rx = 8, ry = 5;
    for (int dy = 0; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            int in = (dx*dx)*(ry*ry) + (dy*dy)*(rx*rx) <= (rx*rx)*(ry*ry);
            if (!in) continue;
            int edge = 0;
            if (dy == 0) edge = 1;
            else {
                int ndxs[] = {-1,1,0,0}, ndys[] = {0,0,-1,1};
                for (int n = 0; n < 4; n++) {
                    int nx = dx+ndxs[n], ny = dy+ndys[n];
                    if (ny < 0) continue;
                    if ((nx*nx)*(ry*ry)+(ny*ny)*(rx*rx) > (rx*rx)*(ry*ry))
                        { edge = 1; break; }
                }
            }
            if (edge) px_set_off(cx+dx, cy+dy);
            else      px_clr_off(cx+dx, cy+dy);
        }
    /* Tongue */
    for (int dy = 1; dy < 5; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dx*dx + dy*dy <= 8)
                px_set_off(cx+dx, cy+ry+dy);
    /* Tongue interior */
    for (int dy = 2; dy < 4; dy++)
        for (int dx = -1; dx <= 1; dx++)
            px_clr_off(cx+dx, cy+ry+dy);
}

/* ─── Excited: star/sparkle pupils, wide open smile ─── */

static void draw_pupils_excited(void) {
    /* Star/sparkle cross-shaped pupils in each eye socket */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        /* Plus/cross shape */
        for (int d = -2; d <= 2; d++) {
            px_set_off(ecx + d, 25);   /* horizontal bar */
            px_set_off(ecx, 25 + d);   /* vertical bar */
        }
        /* Diagonal tips for sparkle */
        px_set_off(ecx - 1, 24); px_set_off(ecx + 1, 24);
        px_set_off(ecx - 1, 26); px_set_off(ecx + 1, 26);
    }
}

static void draw_mouth_excited(void) {
    /* Wide open smile — bigger upward curve than normal */
    for (int x = 22; x < 49; x++) {
        int cy = 37 + ((x - 35) * (x - 35)) / 12;
        px_set_off(x, cy);
        px_set_off(x, cy + 1);
    }
}

/* ─── Nostalgic: pupils looking up-right, gentle half-smile ─── */

static void draw_pupils_nostalgic(void) {
    /* Pupils shifted up and to the right — remembering */
    int centers[][2] = {{24, 23}, {50, 23}};
    for (int e = 0; e < 2; e++)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(centers[e][0]+dx, centers[e][1]+dy);
}

static void draw_mouth_nostalgic(void) {
    /* Gentle closed half-smile — small, wistful */
    for (int x = 31; x < 40; x++) {
        float t = (x - 31) / 8.0f;
        float v = 2.0f * t - 1.0f;
        int y = 40 + (int)(1.5f * v * v);
        px_set_off(x, y);
        px_set_off(x, y + 1);
    }
}

/* ─── Homesick: watery eyes with tears, wobbly mouth ─── */

static void draw_pupils_homesick(void) {
    /* Normal-ish pupils, slightly lowered (sad-like) */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 49 : 23;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 4)
                    px_set_off(ecx+dx, 27+dy);
    }
}

static void draw_tears_homesick(void) {
    /* Tear drop pixels below each eye socket */
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        px_set_off(ecx, 31);
        px_set_off(ecx, 32);
        px_set_off(ecx, 33);
        px_set_off(ecx - 1, 32);
        px_set_off(ecx + 1, 32);
    }
}

static void draw_mouth_homesick(void) {
    /* Wobbly trying-not-to-cry line — slightly wavy horizontal */
    for (int x = 28; x < 43; x++) {
        float t = (x - 28) / 14.0f;
        int y = 40 + (int)(1.5f * sinf(t * 3.14159f * 3.0f));
        px_set_off(x, y);
        px_set_off(x, y + 1);
    }
}

/* ─── Body animation transform, set per-frame before rendering ─── */
static void setup_body_transform(uint8_t mood, uint32_t f) {
    /* Reset */
    body_dx = 0; body_dy = 0; body_x_expand = 0;
    wobble_amp = 0; wobble_freq = 0; wobble_phase = 0;

    float pi = 3.14159f;
    switch (mood) {
        case MOOD_ANGRY:
            body_dy = -1; body_x_expand = 2;
            wobble_amp = 0.5f; wobble_freq = 0.3f; wobble_phase = f * pi;
            break;
        case MOOD_SAD:
            body_dy = 3; body_x_expand = -1;
            break;
        case MOOD_UNHINGED:
            body_dx = (int)(1.5f * sinf(f * 7.3f));
            body_dy = (int)(1.5f * sinf(f * 5.1f + 1));
            break;
        case MOOD_WEIRD:
            body_dx = (int)(3 * sinf(f * 0.8f));
            wobble_amp = 1.5f; wobble_freq = 0.15f; wobble_phase = (float)f;
            break;
        case MOOD_CHAOTIC:
            body_dx = (int)(2 * sinf(f * 2.1f));
            body_dy = (int)(2 * sinf(f * 1.7f));
            wobble_amp = 3; wobble_freq = 0.25f; wobble_phase = f * 2.0f;
            break;
        case MOOD_HUNGRY:
            body_dy = -2 + (int)sinf(f * 1.5f);
            break;
        case MOOD_TIRED:
            body_dy = 2 + (int)sinf(f * 0.5f); body_x_expand = -1;
            break;
        case MOOD_SLAPHAPPY:
            body_dx = (int)(3 * sinf(f * 1.2f));
            wobble_amp = 2; wobble_freq = 0.1f; wobble_phase = f * 1.2f;
            break;
        case MOOD_LAZY:
            body_dy = 3; body_x_expand = 3;
            break;
        case MOOD_FAT:
            body_x_expand = 3; body_dy = (int)sinf(f * 1.8f);
            break;
        case MOOD_CHILL:
            body_dx = (int)sinf(f * 0.4f); body_dy = 1;
            break;
        case MOOD_CREEPY:
            body_x_expand = (int)(2 * sinf(f * 2.0f));
            break;
        case MOOD_EXCITED:
            body_dy = (int)(3 * sinf(f * 3.0f));
            break;
        case MOOD_NOSTALGIC:
            body_dx = (int)(2 * sinf(f * 0.5f));
            body_dy = (int)sinf(f * 0.3f);
            break;
        case MOOD_HOMESICK:
            body_dy = 1; body_x_expand = -2;
            break;
        case MOOD_WISE:
            /* Slow professorial nod + tiny sway, as if mid-lecture */
            body_dy = (int)sinf(f * 0.5f);
            body_dx = (int)(1.5f * sinf(f * 0.3f));
            break;
        default: /* NORMAL: gentle breathing */
            body_dy = (int)sinf(f * 0.8f);
            break;
    }
}

/* Draw just the octopus (body + eyes + pupils + brows + mouth) at the current
 * layout origin. Shared by the wide and tall layouts. */
void draw_octopus(const Quote *q, int expr, uint32_t frame_idx) {
    /* Set up body animation transform for this frame */
    setup_body_transform(q->mood, frame_idx);

    /* Body (with Y_OFF + body transform) */
    draw_body_transformed();

    /* Eyes (white sockets, with Y_OFF) */
    draw_eyes();

    /* Pupils (mood-specific, with Y_OFF) — a scripted gaze wins over the mood */
    if (g_gaze_override) {
        draw_pupils_gaze(g_gaze_dx, g_gaze_dy);
    } else switch (q->mood) {
        case MOOD_WEIRD:    draw_pupils_weird();    break;
        case MOOD_UNHINGED: draw_pupils_unhinged(); break;
        case MOOD_ANGRY:    draw_pupils_angry();    break;
        case MOOD_SAD:      draw_pupils_sad();      break;
        case MOOD_CHAOTIC:  draw_pupils_chaotic();  break;
        case MOOD_HUNGRY:   draw_pupils_hungry();   break;
        case MOOD_TIRED:    draw_pupils_tired();    break;
        case MOOD_LAZY:     draw_pupils_lazy();     break;
        case MOOD_FAT:      draw_pupils_fat();      break;
        case MOOD_CHILL:    draw_pupils_chill();    break;
        case MOOD_CREEPY:    draw_pupils_creepy();    break;
        case MOOD_EXCITED:  draw_pupils_excited();  break;
        case MOOD_NOSTALGIC: draw_pupils_nostalgic(); break;
        case MOOD_HOMESICK: draw_pupils_homesick(); break;
        case MOOD_WISE:     draw_pupils_wise();     break;
        default:            draw_pupils_normal();   break;
    }

    /* 3b. Eyebrows / eyelids / special eyes */
    if (q->mood == MOOD_ANGRY)     draw_brows_angry();
    if (q->mood == MOOD_SAD)       draw_brows_sad();
    if (q->mood == MOOD_TIRED)     draw_lids_tired();
    if (q->mood == MOOD_SLAPHAPPY) draw_eyes_slaphappy();
    if (q->mood == MOOD_LAZY)      draw_lids_lazy();
    if (q->mood == MOOD_HOMESICK)  draw_tears_homesick();
    if (q->mood == MOOD_WISE)      draw_glasses_wise();

    /* 4. Mouth expression (with Y_OFF) */
    switch (expr) {
        case EXPR_OPEN:      draw_mouth_open();      break;
        case EXPR_SMILE:     draw_mouth_smile();     break;
        case EXPR_WEIRD:     draw_mouth_weird();     break;
        case EXPR_UNHINGED:  draw_mouth_unhinged();  break;
        case EXPR_ANGRY:     draw_mouth_angry();     break;
        case EXPR_SAD:       draw_mouth_sad();       break;
        case EXPR_CHAOTIC:   draw_mouth_chaotic();   break;
        case EXPR_HUNGRY:    draw_mouth_hungry();    break;
        case EXPR_TIRED:     draw_mouth_tired();     break;
        case EXPR_SLAPHAPPY: draw_mouth_slaphappy(); break;
        case EXPR_LAZY:      draw_mouth_lazy();      break;
        case EXPR_FAT:       draw_mouth_fat();       break;
        case EXPR_CHILL:     draw_mouth_chill();     break;
        case EXPR_CREEPY:     draw_mouth_creepy();     break;
        case EXPR_EXCITED:   draw_mouth_excited();   break;
        case EXPR_NOSTALGIC: draw_mouth_nostalgic(); break;
        case EXPR_HOMESICK:  draw_mouth_homesick();  break;
        case EXPR_WISE:      draw_mouth_wise();      break;
        default:             draw_mouth_smirk();     break;
    }

}

/* ─── Sleeping Greg (STATE_SLEEP still) ─────────────────────────────────────
 * Old-cartoon nightcap, in body-local coords (px_*_off): a white band rolled
 * across the dome, a striped cone drooping to the right, pom-pom on the tip.
 * White-with-black-outline so it reads against the solid-black head. Spans
 * follow the body silhouette EXPANDED BY 2 (draw_greg_sleeping sets
 * body_x_expand=2), so the band meets the head edge exactly. */
static void draw_sleep_cap(void) {
    /* Band: white roll over the top dome rows. Bottom edge stays black body. */
    static const uint8_t band[6][3] = {
        {10,20,50}, {11,16,54}, {12,14,56}, {13,12,58}, {14,11,59}, {15,10,60}
    };
    for (int i = 0; i < 6; i++) {
        int y = band[i][0], x0 = band[i][1], x1 = band[i][2];
        for (int x = x0; x <= x1; x++) px_clr_off(x, y);
        px_set_off(x0, y); px_set_off(x0 + 1, y);
        px_set_off(x1 - 1, y); px_set_off(x1, y);
    }
    /* Rolled brim: two black rows separating band and cone. */
    for (int x = 20; x <= 48; x++) px_set_off(x, 8);
    for (int x = 21; x <= 47; x++) px_set_off(x, 9);
    /* Cone: leans right from the brim up to the tip, outlined, white inside. */
    for (int y = 7; y >= -4; y--) {
        int d  = 7 - y;                       /* 0 at the brim, 11 at the tip */
        int xl = 22 + (d * 31) / 11;          /* 22 → 53 */
        int xr = 46 + (d * 15) / 11;          /* 46 → 61 */
        for (int x = xl; x <= xr; x++) px_clr_off(x, y);
        px_set_off(xl, y); px_set_off(xl + 1, y);
        px_set_off(xr - 1, y); px_set_off(xr, y);
        if (y == 1 || y == 4)                 /* the classic stripes */
            for (int x = xl + 2; x <= xr - 2; x++) px_set_off(x, y);
    }
    /* Pom-pom on the tip: outlined white ball with a few fluff dots. */
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= 16) px_clr_off(58 + dx, -6 + dy);
            if (d2 <= 16 && d2 >= 10) px_set_off(58 + dx, -6 + dy);
        }
    px_set_off(56, -7); px_set_off(59, -6); px_set_off(57, -4);
}

/* Closed eyes: white sockets mostly lidded over, a white crescent left under
 * each lash line — the classic peaceful-sleeper look. */
static void draw_eyes_sleeping(void) {
    for (int e = 0; e < 2; e++) {
        int ecx = e ? 48 : 22;
        for (int dy = -4; dy <= 1; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx * dx + dy * dy <= 16)
                    px_set_off(ecx + dx, 25 + dy);
    }
}

/* Greg asleep: relaxed splat (no wobble), lidded eyes, tiny snore mouth, cap. */
void draw_greg_sleeping(void) {
    body_dx = 0; body_dy = 0; body_x_expand = 2;
    wobble_amp = 0; wobble_freq = 0; wobble_phase = 0;
    draw_body_transformed();
    draw_eyes();
    draw_eyes_sleeping();
    fill_circle(35, 42, 5, 0);        /* small round snore mouth */
    draw_sleep_cap();
    body_x_expand = 0;
}

/* ─── Expression cycles per mood ─── */

static const uint8_t cycle_normal[]   = {EXPR_SMIRK, EXPR_OPEN, EXPR_SMILE, EXPR_OPEN};
static const uint8_t cycle_weird[]    = {EXPR_WEIRD, EXPR_OPEN, EXPR_WEIRD, EXPR_SMILE};
static const uint8_t cycle_unhinged[] = {EXPR_UNHINGED, EXPR_OPEN, EXPR_UNHINGED, EXPR_OPEN};
static const uint8_t cycle_angry[]     = {EXPR_ANGRY, EXPR_OPEN, EXPR_ANGRY, EXPR_ANGRY};
static const uint8_t cycle_sad[]       = {EXPR_SAD, EXPR_OPEN, EXPR_SAD, EXPR_SMILE};
static const uint8_t cycle_chaotic[]   = {EXPR_CHAOTIC, EXPR_OPEN, EXPR_UNHINGED, EXPR_WEIRD};
static const uint8_t cycle_hungry[]    = {EXPR_HUNGRY, EXPR_OPEN, EXPR_HUNGRY, EXPR_SMILE};
static const uint8_t cycle_tired[]     = {EXPR_TIRED, EXPR_OPEN, EXPR_TIRED, EXPR_TIRED};
static const uint8_t cycle_slaphappy[] = {EXPR_SLAPHAPPY, EXPR_OPEN, EXPR_SLAPHAPPY, EXPR_SMILE};
static const uint8_t cycle_lazy[]      = {EXPR_LAZY, EXPR_LAZY, EXPR_LAZY, EXPR_OPEN};
static const uint8_t cycle_fat[]       = {EXPR_FAT, EXPR_OPEN, EXPR_FAT, EXPR_SMILE};
static const uint8_t cycle_chill[]     = {EXPR_CHILL, EXPR_OPEN, EXPR_CHILL, EXPR_SMILE};
static const uint8_t cycle_creepy[]     = {EXPR_CREEPY, EXPR_OPEN, EXPR_CREEPY, EXPR_SMILE};
static const uint8_t cycle_excited[]   = {EXPR_EXCITED, EXPR_OPEN, EXPR_EXCITED, EXPR_SMILE};
static const uint8_t cycle_nostalgic[] = {EXPR_NOSTALGIC, EXPR_OPEN, EXPR_NOSTALGIC, EXPR_SMILE};
static const uint8_t cycle_homesick[]  = {EXPR_HOMESICK, EXPR_OPEN, EXPR_HOMESICK, EXPR_HOMESICK};
static const uint8_t cycle_wise[]      = {EXPR_WISE, EXPR_OPEN, EXPR_WISE, EXPR_SMILE};

const uint8_t *mood_cycle(uint8_t mood) {
    switch (mood) {
        case MOOD_WEIRD:     return cycle_weird;
        case MOOD_UNHINGED:  return cycle_unhinged;
        case MOOD_ANGRY:     return cycle_angry;
        case MOOD_SAD:       return cycle_sad;
        case MOOD_CHAOTIC:   return cycle_chaotic;
        case MOOD_HUNGRY:    return cycle_hungry;
        case MOOD_TIRED:     return cycle_tired;
        case MOOD_SLAPHAPPY: return cycle_slaphappy;
        case MOOD_LAZY:      return cycle_lazy;
        case MOOD_FAT:       return cycle_fat;
        case MOOD_CHILL:     return cycle_chill;
        case MOOD_CREEPY:     return cycle_creepy;
        case MOOD_EXCITED:   return cycle_excited;
        case MOOD_NOSTALGIC: return cycle_nostalgic;
        case MOOD_HOMESICK:  return cycle_homesick;
        case MOOD_WISE:      return cycle_wise;
        default:             return cycle_normal;
    }
}
