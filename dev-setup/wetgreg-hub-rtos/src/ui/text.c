/**
 * text.c — 5×7 bitmap font + text rendering.
 *
 * Purpose: hold the 5x7 font tables and implement the text renderers; the
 * tables stay private behind font_glyph().
 */
#include "text.h"

#include <string.h>

#include "canvas.h"

/* ─── 5×7 bitmap font ─── */
/* Index: A=0..Z=25, 0=26..9=35, ' '=36, .=37, ,=38, !=39, ?=40,
   '=41, -=42, ~=43, /=44, :=45, (=46, )=47, %=48 */
static const uint8_t font5x7[][7] = {
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, /* A */
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, /* B */
    {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, /* C */
    {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}, /* D */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, /* E */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, /* F */
    {0x0e,0x11,0x10,0x17,0x11,0x11,0x0e}, /* G */
    {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, /* H */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0c}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, /* L */
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, /* O */
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}, /* P */
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, /* Q */
    {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, /* R */
    {0x0e,0x11,0x10,0x0e,0x01,0x11,0x0e}, /* S */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, /* U */
    {0x11,0x11,0x11,0x11,0x0a,0x0a,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, /* W */
    {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, /* X */
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, /* Y */
    {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}, /* Z */
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, /* 0 */
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, /* 1 */
    {0x0e,0x11,0x01,0x06,0x08,0x10,0x1f}, /* 2 */
    {0x0e,0x11,0x01,0x06,0x01,0x11,0x0e}, /* 3 */
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, /* 4 */
    {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}, /* 5 */
    {0x0e,0x11,0x10,0x1e,0x11,0x11,0x0e}, /* 6 */
    {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, /* 8 */
    {0x0e,0x11,0x11,0x0f,0x01,0x11,0x0e}, /* 9 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c}, /* . */
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* , */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
    {0x0e,0x11,0x01,0x06,0x04,0x00,0x04}, /* ? */
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00}, /* ' */
    {0x00,0x00,0x00,0x1f,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, /* ~ */
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, /* / */
    {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}, /* : */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
    {0x19,0x1a,0x02,0x04,0x08,0x0b,0x13}, /* % */
};

static const char font_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,!?'-~/:()\%";

static int font_index(char c) {
    for (int i = 0; font_chars[i]; i++)
        if (font_chars[i] == c) return i;
    return 36; /* space fallback */
}

const uint8_t *font_glyph(char c) {
    if (c == '\0') return NULL;
    if (c >= 'a' && c <= 'z') c -= 32;
    const char *pos = strchr(font_chars, c);
    return pos ? font5x7[(int)(pos - font_chars)] : NULL;
}

/* ─── Drawing ─── */

static void draw_char(int x0, int y0, int idx) {
    for (int row = 0; row < 7; row++) {
        uint8_t bits = font5x7[idx][row];
        for (int col = 0; col < 5; col++)
            if (bits & (0x10 >> col))
                px_set(x0 + col, y0 + row);
    }
}

void draw_char_c(int x0, int y0, char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    draw_char(x0, y0, font_index(c));
}

void draw_text(int x0, int y0, const char *text, int max_w) {
    int cx = x0, cy = y0;
    int char_w = 6; /* 5px + 1px gap */

    /* Simple word-wrap */
    const char *p = text;
    while (*p) {
        /* Measure next word */
        int wlen = 0;
        while (p[wlen] && p[wlen] != ' ') wlen++;

        int word_px = wlen * char_w;

        /* Wrap if this word won't fit on current line */
        if (cx > x0 && (cx - x0) + word_px > max_w) {
            cx = x0;
            cy += 9; /* 7px + 2px line gap */
        }

        /* Render the word */
        for (int i = 0; i < wlen; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') c -= 32; /* uppercase */
            draw_char(cx, cy, font_index(c));
            cx += char_w;
        }

        p += wlen;
        /* Skip spaces */
        if (*p == ' ') {
            cx += char_w;
            p++;
        }
    }
}

/* Double-size text (each font pixel → 2x2 block); no wrap. Used for the BLE passkey. */
void draw_text_2x(int x0, int y0, const char *text) {
    int cx = x0;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        int idx = font_index(c);
        for (int row = 0; row < 7; row++) {
            uint8_t bits = font5x7[idx][row];
            for (int col = 0; col < 5; col++)
                if (bits & (0x10 >> col)) {
                    int px = cx + col * 2, py = y0 + row * 2;
                    px_set(px, py);     px_set(px + 1, py);
                    px_set(px, py + 1); px_set(px + 1, py + 1);
                }
        }
        cx += 16;   /* 5px*2 + 2px*2 gap */
    }
}

/* Word-wrapped double-size text: draw_text's wrap logic with draw_text_2x's
 * 2x2 glyph scaling, on a tighter 12 px advance so real words fit the 122 px
 * canvas. Used by the intro story. */
void draw_text_big(int x0, int y0, const char *text, int max_w) {
    int cx = x0, cy = y0;
    const int char_w = 12;              /* 10 px glyph + 2 px gap */

    const char *p = text;
    while (*p) {
        int wlen = 0;
        while (p[wlen] && p[wlen] != ' ') wlen++;
        int word_px = wlen * char_w;

        if (cx > x0 && (cx - x0) + word_px > max_w) {
            cx = x0;
            cy += 18;                   /* 14 px glyph + 4 px line gap */
        }

        for (int i = 0; i < wlen; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            int idx = font_index(c);
            for (int row = 0; row < 7; row++) {
                uint8_t bits = font5x7[idx][row];
                for (int col = 0; col < 5; col++)
                    if (bits & (0x10 >> col)) {
                        int px = cx + col * 2, py = cy + row * 2;
                        px_set(px, py);     px_set(px + 1, py);
                        px_set(px, py + 1); px_set(px + 1, py + 1);
                    }
            }
            cx += char_w;
        }

        p += wlen;
        if (*p == ' ') { cx += char_w; p++; }
    }
}

/* Pseudo-bold small text: double-struck 1 px apart (no wrap). */
void draw_text_bold(int x0, int y0, const char *text) {
    draw_text(x0,     y0, text, 1000);
    draw_text(x0 + 1, y0, text, 1000);
}

/* ─── Helper: draw inverted text (white on black bar) ─── */
void draw_inverted_line(int y, const char *text) {
    int hx_end = canvas_w - 6;          /* canvas-aware: 244 wide, 116 tall */
    for (int hy = y - 1; hy < y + 8; hy++)
        for (int hx = 6; hx < hx_end; hx++)
            px_set(hx, hy);
    int cx = 10;
    for (const char *c = text; *c; c++) {
        char up = *c;
        if (up >= 'a' && up <= 'z') up -= 32;
        const char *pos = strchr(font_chars, up);
        if (pos) {
            int idx = (int)(pos - font_chars);
            for (int row = 0; row < 7; row++) {
                uint8_t bits = font5x7[idx][row];
                for (int col = 0; col < 5; col++)
                    if (bits & (0x10 >> col))
                        px_clr(cx + col, y + row);
            }
        }
        cx += 6;
    }
}
