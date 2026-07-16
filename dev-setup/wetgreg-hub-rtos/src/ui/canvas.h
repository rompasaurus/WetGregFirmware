/**
 * canvas.h — the logical 1-bpp drawing canvas.
 *
 * Purpose: define the shared drawing surface (buffer, dimensions, pixel ops)
 * that every renderer draws into.
 *
 * Two shapes share one buffer:
 *   WIDE  250x122 (row bytes 32) — side-by-side octopus screen + all menus
 *   TALL  122x250 (row bytes 16) — the "longways" stacked layout
 * transpose_to_display() (drivers/display.h) rotates whichever canvas is
 * active onto the fixed 122x250 panel. Buffer is sized for the larger
 * (tall: 16*250 = 4000).
 */
#ifndef CANVAS_H
#define CANVAS_H

#include <stdint.h>

/* ─── Canvas constants (the WIDE default) ─── */
#define IMG_W         250
#define IMG_H         122
#define IMG_ROW_BYTES ((IMG_W + 7) / 8)  /* 32 */

#define CANVAS_BYTES_MAX 4000

/* Shared drawing state — written by set_canvas_wide/tall, read by every
 * renderer. UI-task only (all rendering happens on the UI task). */
extern uint8_t frame[CANVAS_BYTES_MAX];   /* 1 = black pixel, packed MSB-first */
extern int canvas_w;
extern int canvas_h;
extern int canvas_row_bytes;

/* Panel-map angle used by transpose_to_display(): 90/270 = wide canvas,
 * 0/180 = tall canvas. Chosen by set_canvas_*() from the orientation. */
extern int display_rotation;

void set_canvas_wide(void);
void set_canvas_tall(void);

/* Blank full canvas rows [y0..y1] (clamped). */
void clear_rows(int y0, int y1);

/* ─── Pixel helpers ─── */
static inline void px_set(int x, int y) {
    if (x >= 0 && x < canvas_w && y >= 0 && y < canvas_h)
        frame[y * canvas_row_bytes + x / 8] |= (0x80 >> (x & 7));
}
static inline void px_clr(int x, int y) {
    if (x >= 0 && x < canvas_w && y >= 0 && y < canvas_h)
        frame[y * canvas_row_bytes + x / 8] &= ~(0x80 >> (x & 7));
}

#endif /* CANVAS_H */
