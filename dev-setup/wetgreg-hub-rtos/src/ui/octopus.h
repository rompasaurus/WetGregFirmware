/**
 * octopus.h — Greg the octopus: runtime-rendered body/eyes/mouth art, mood
 * animation transforms, expression cycles, and the sleeping still.
 *
 * Purpose: expose Greg's face: moods/expressions, the draw calls, and the
 * animation hooks (gaze, swim, layout origin).
 *
 * Instead of pre-baking every frame, each frame is composited at display
 * time: body, eyes, mood-specific pupils/brows, mouth expression — so ALL
 * quotes fit in flash (~10KB of strings vs ~4MB of bitmaps).
 */
#ifndef OCTOPUS_H
#define OCTOPUS_H

#include <stdint.h>

#include "quotes.h"   /* the Quote type. NOTE: quotes.h defines the quote
                       * table as a static array in the header — ONLY
                       * app_main.c may reference `quotes[]`; every other
                       * includer uses just the type + QUOTE_COUNT. */

/* Mood values (match quotes.h mood_map in devtool.py) */
#define MOOD_NORMAL    0
#define MOOD_WEIRD     1
#define MOOD_UNHINGED  2
#define MOOD_ANGRY     3
#define MOOD_SAD       4
#define MOOD_CHAOTIC   5
#define MOOD_HUNGRY    6
#define MOOD_TIRED     7
#define MOOD_SLAPHAPPY 8
#define MOOD_LAZY      9
#define MOOD_FAT       10
#define MOOD_CHILL     11
#define MOOD_CREEPY    12
#define MOOD_EXCITED   13
#define MOOD_NOSTALGIC 14
#define MOOD_HOMESICK  15
#define MOOD_WISE      16
#define MOOD_COUNT     17

/* Mouth expressions */
#define EXPR_SMIRK     0
#define EXPR_OPEN      1
#define EXPR_SMILE     2
#define EXPR_WEIRD     3
#define EXPR_UNHINGED  4
#define EXPR_ANGRY     5
#define EXPR_SAD       6
#define EXPR_CHAOTIC   7
#define EXPR_HUNGRY    8
#define EXPR_TIRED     9
#define EXPR_SLAPHAPPY 10
#define EXPR_LAZY      11
#define EXPR_FAT       12
#define EXPR_CHILL     13
#define EXPR_CREEPY    14
#define EXPR_EXCITED   15
#define EXPR_NOSTALGIC 16
#define EXPR_HOMESICK  17
#define EXPR_WISE      18

/* Mood names for the selector / tags. */
extern const char *const mood_names[MOOD_COUNT];

/* Current mood filter: -1 = all moods (random quote pick). */
extern int current_mood;

/* Vertical offset — pushes octopus + bubble down to make room for the clock. */
#define Y_OFF 12

/* Layout origin — lets the octopus art be repositioned (e.g. bottom of the
 * tall canvas) without touching any of the drawing routines. Set it before
 * draw_octopus()/draw_greg_sleeping() and reset to 0,0 after. */
extern int layout_ox, layout_oy;

/* Draw the octopus (body + eyes + pupils + brows + mouth) at the current
 * layout origin. `q->mood` picks the face; `expr` the mouth; `frame_idx`
 * drives the body animation. Shared by the wide and tall layouts. */
void draw_octopus(const Quote *q, int expr, uint32_t frame_idx);

/* The 4-frame mouth-expression cycle for a mood. */
const uint8_t *mood_cycle(uint8_t mood);

/* Scriptable gaze (intro animation): overrides the mood's pupils with
 * normal pupils offset by (dx,dy) until cleared, so a state can direct
 * WHERE Greg looks frame by frame. */
void octopus_gaze_set(int dx, int dy);
void octopus_gaze_clear(void);

/* Tentacle swim-kick (intro): a traveling wave down the tentacle rows,
 * anchored at the body and growing toward the tips. amp 0 disables. */
void octopus_swim_set(float amp, float phase);

/* Greg asleep: relaxed splat, lidded eyes, snore mouth, nightcap
 * (the STATE_SLEEP still). */
void draw_greg_sleeping(void);

#endif /* OCTOPUS_H */
