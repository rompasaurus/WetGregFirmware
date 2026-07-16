/**
 * text.h — 5×7 bitmap font + text rendering onto the canvas.
 *
 * Purpose: provide all text rendering (normal/2x/big/bold/inverted) and
 * font-glyph access on top of the canvas.
 *
 * Charset: A-Z 0-9 space . , ! ? ' - ~ / : ( ) %  (lowercase is uppercased).
 */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

/* The 7-row glyph bitmap for `c` (lowercase folded), or NULL if the
 * character is not in the font. Rows are 5 bits wide, MSB-first from 0x10. */
const uint8_t *font_glyph(char c);

/* One glyph at (x0,y0); unmapped characters render as a space. */
void draw_char_c(int x0, int y0, char c);

/* Word-wrapped text, 6 px advance, wraps at max_w. */
void draw_text(int x0, int y0, const char *text, int max_w);

/* Double-size text (each font pixel → 2x2 block); no wrap. 16 px advance. */
void draw_text_2x(int x0, int y0, const char *text);

/* Word-wrapped double-size text on a tighter 12 px advance. */
void draw_text_big(int x0, int y0, const char *text, int max_w);

/* Pseudo-bold small text: double-struck 1 px apart (no wrap). */
void draw_text_bold(int x0, int y0, const char *text);

/* Menu row: white text on a full-width black bar at row y. */
void draw_inverted_line(int y, const char *text);

#endif /* TEXT_H */
