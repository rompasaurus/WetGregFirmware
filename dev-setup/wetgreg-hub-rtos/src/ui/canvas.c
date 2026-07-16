/**
 * canvas.c — the logical drawing canvas: buffer, shape switching, row clears.
 *
 * Purpose: hold the canvas buffer and switch its wide/tall shape (picking
 * the panel rotation) as the orientation changes.
 */
#include "canvas.h"

#include <string.h>

#include "motion.h"   /* orientation_is_tall() + ORIENT_CFG / g_orientation */

uint8_t frame[CANVAS_BYTES_MAX];
int canvas_w         = IMG_W;          /* default WIDE */
int canvas_h         = IMG_H;
int canvas_row_bytes = IMG_ROW_BYTES;

/* display_rotation is the panel-map angle (see transpose_to_display). */
int display_rotation = 90;

void set_canvas_wide(void) {
    canvas_w = 250; canvas_h = 122; canvas_row_bytes = 32;
    /* Wide screens must use a wide angle (90/270); a tall hold viewing a wide
     * menu falls back to 90 until the tall-menu rework lands. */
    display_rotation = orientation_is_tall() ? 90 : ORIENT_CFG[g_orientation].disp_rot;
}

void set_canvas_tall(void) {
    canvas_w = 122; canvas_h = 250; canvas_row_bytes = 16;
    display_rotation = ORIENT_CFG[g_orientation].disp_rot;
}

/* Blank full canvas rows [y0..y1] — e.g. the sleep still wipes the bubble
 * scatter out of its text bands, so a randomly-placed bubble can't spend the
 * whole nap tangled in the title (on a STILL that reads as a rendering
 * glitch). */
void clear_rows(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 >= canvas_h) y1 = canvas_h - 1;
    if (y0 > y1) return;
    memset(&frame[y0 * canvas_row_bytes], 0, (size_t)(y1 - y0 + 1) * canvas_row_bytes);
}
