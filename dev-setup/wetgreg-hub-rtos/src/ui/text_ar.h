/**
 * text_ar.h — Arabic text rendering (UTF-8 in, shaped right-to-left out).
 *
 * Purpose: draw Egyptian-Arabic strings with the generated Kufi bitmap fonts:
 * UTF-8 decode → contextual letter forms (initial/medial/final/isolated and
 * the lam-alef ligatures) → word-wrapped lines laid out right-to-left, with
 * digit/Latin runs kept left-to-right inside the line.
 *
 * Three sizes live in font_ar.h: AR_FONT_UI (11 px) for menus and labels,
 * AR_FONT_Q (13 px) for the quote bubble, AR_FONT_BIG (18 px) for titles and
 * the intro story. Charset: Arabic letters, Arabic-Indic digits ٠-٩, ، ؟ and
 * space from the Arabic font; ASCII digits, A-Z and . - ! % : fall back to the
 * 5×7 Latin font sitting on the Arabic baseline. Anything else is blank.
 *
 * The UI hooks at the bottom are what text.c calls so EVERY existing
 * draw_text()/draw_inverted_line() site becomes Arabic automatically when the
 * language is set: the English literal is looked up in i18n.c, the layout is
 * mirrored (right-aligned at canvas_w - x), and "> " cursor prefixes move to
 * the right edge.
 */
#ifndef TEXT_AR_H
#define TEXT_AR_H

#include <stdint.h>

typedef struct ar_font ar_font_t;    /* opaque here; defined in font_ar.h */
extern const ar_font_t *const AR_FONT_UI;
extern const ar_font_t *const AR_FONT_Q;
extern const ar_font_t *const AR_FONT_BIG;

/* Metrics of any font: cell rows, baseline row, a sensible line pitch. Layout
 * code derives its geometry from these so a font-size change is one regen. */
int ar_font_rows(const ar_font_t *f);
int ar_font_baseline(const ar_font_t *f);
int ar_font_pitch(const ar_font_t *f);

/* Word-wrapped text with its right edge at x_right (RTL), first line's cell
 * top at y0, wrapping at max_w px, at most max_lines lines (the rest is
 * dropped). invert=1 clears pixels instead of setting them (for highlight
 * bars). Returns the number of lines drawn. */
int  draw_text_ar_f(const ar_font_t *f, int x_right, int y0, const char *utf8,
                    int max_w, int max_lines, int invert);
int  text_ar_width_f(const ar_font_t *f, const char *utf8);

/* Quote-font conveniences (the original API). */
int  draw_text_ar(int x_right, int y0, const char *utf8, int max_w, int max_lines);
int  text_ar_width(const char *utf8);
void draw_text_ar_centered(int x0, int w, int y0, const char *utf8);

/* Write `v` as Arabic-Indic digits (UTF-8) into buf; returns strlen. */
int  ar_itoa(char *buf, int n, unsigned v);

/* Copy the first `n_cps` code points of `utf8` into out (typewriter reveal). */
void ar_utf8_prefix(const char *utf8, int n_cps, char *out, int out_n);
int  ar_utf8_len(const char *utf8);                 /* code points */

/* ─── Hooks for text.c (return 1 when they drew the text in Arabic) ─── */
int  ar_ui_text(int x, int y, const char *text, int max_w);      /* draw_text */
int  ar_ui_inverted(int y, const char *text);                    /* draw_inverted_line */
int  ar_ui_big(int x, int y, const char *text, int max_w);       /* draw_text_big */
int  ar_ui_bold(int x, int y, const char *text);                 /* draw_text_bold */

#endif /* TEXT_AR_H */
