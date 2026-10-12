/**
 * text_ar.c — Arabic text rendering: UTF-8 → shaped glyphs → RTL layout.
 *
 * Purpose: implement draw_text_ar_f() and the text.c hooks on top of the
 * generated font_ar.h tables (Noto Kufi Arabic rasterised to 1-bit by
 * tools/arfont.py in three sizes).
 *
 * The shaping is the simple Unicode joining model: every letter has up to
 * four forms (isolated / final / initial / medial) and a letter takes a
 * joined form only when the neighbour on that side can join. Lam followed by
 * any alef becomes the mandatory lam-alef ligature. That covers the whole
 * quote + UI charset; no diacritics are supported (the strings carry none).
 */
#include "text_ar.h"

#include <string.h>

#include "canvas.h"
#include "font_ar.h"
#include "i18n.h"
#include "storage.h"     /* lang_is_ar */
#include "text.h"

const ar_font_t *const AR_FONT_UI  = &font_ar_ui;
const ar_font_t *const AR_FONT_Q   = &font_ar_q;
const ar_font_t *const AR_FONT_BIG = &font_ar_big;

#define MAX_CPS   160     /* longest string we lay out (quotes are ≤ 80 chars) */
#define MAX_WORDS 48

int ar_font_rows(const ar_font_t *f)     { return f->rows; }
int ar_font_baseline(const ar_font_t *f) { return f->baseline; }
int ar_font_pitch(const ar_font_t *f)    { return f->rows - 2; }

/* ─── Shaping table: U+0621..U+064A → {isolated, final, initial, medial} ─── */
#define AR_FIRST 0x0621
#define AR_LAST  0x064A
static const uint16_t ar_forms[AR_LAST - AR_FIRST + 1][4] = {
    {0xFE80, 0,      0,      0     },  /* ء */
    {0xFE81, 0xFE82, 0,      0     },  /* آ */
    {0xFE83, 0xFE84, 0,      0     },  /* أ */
    {0xFE85, 0xFE86, 0,      0     },  /* ؤ */
    {0xFE87, 0xFE88, 0,      0     },  /* إ */
    {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},  /* ئ */
    {0xFE8D, 0xFE8E, 0,      0     },  /* ا */
    {0xFE8F, 0xFE90, 0xFE91, 0xFE92},  /* ب */
    {0xFE93, 0xFE94, 0,      0     },  /* ة */
    {0xFE95, 0xFE96, 0xFE97, 0xFE98},  /* ت */
    {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},  /* ث */
    {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},  /* ج */
    {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},  /* ح */
    {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},  /* خ */
    {0xFEA9, 0xFEAA, 0,      0     },  /* د */
    {0xFEAB, 0xFEAC, 0,      0     },  /* ذ */
    {0xFEAD, 0xFEAE, 0,      0     },  /* ر */
    {0xFEAF, 0xFEB0, 0,      0     },  /* ز */
    {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},  /* س */
    {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},  /* ش */
    {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},  /* ص */
    {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},  /* ض */
    {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},  /* ط */
    {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},  /* ظ */
    {0xFEC9, 0xFECA, 0xFECB, 0xFECC},  /* ع */
    {0xFECD, 0xFECE, 0xFECF, 0xFED0},  /* غ */
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},   /* U+063B..063D unused */
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},   /* U+063E..0640 (incl. tatweel) */
    {0xFED1, 0xFED2, 0xFED3, 0xFED4},  /* ف */
    {0xFED5, 0xFED6, 0xFED7, 0xFED8},  /* ق */
    {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},  /* ك */
    {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},  /* ل */
    {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},  /* م */
    {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},  /* ن */
    {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},  /* ه */
    {0xFEED, 0xFEEE, 0,      0     },  /* و */
    {0xFEEF, 0xFEF0, 0,      0     },  /* ى */
    {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},  /* ي */
};

static inline const uint16_t *forms_of(uint32_t cp) {
    if (cp < AR_FIRST || cp > AR_LAST) return NULL;
    const uint16_t *f = ar_forms[cp - AR_FIRST];
    return f[0] ? f : NULL;
}
static inline int joins_left (uint32_t cp) { const uint16_t *f = forms_of(cp); return f && f[2]; }
static inline int joins_right(uint32_t cp) { const uint16_t *f = forms_of(cp); return f && f[1]; }

/* lam + {آ أ إ ا} → {isolated, final} ligature */
static int lam_alef(uint32_t alef, uint16_t *iso, uint16_t *fin) {
    switch (alef) {
        case 0x0622: *iso = 0xFEF5; *fin = 0xFEF6; return 1;
        case 0x0623: *iso = 0xFEF7; *fin = 0xFEF8; return 1;
        case 0x0625: *iso = 0xFEF9; *fin = 0xFEFA; return 1;
        case 0x0627: *iso = 0xFEFB; *fin = 0xFEFC; return 1;
        default: return 0;
    }
}

/* ─── UTF-8 → code points ─── */
static int utf8_decode(const char *s, uint16_t *out, int max) {
    int n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n < max) {
        uint32_t cp; int len;
        if      (*p < 0x80)          { cp = *p;        len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { p++; continue; }                       /* stray continuation byte */
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = i; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += len;
        out[n++] = (uint16_t)(cp > 0xFFFF ? 0x20 : cp);
    }
    return n;
}

int ar_utf8_len(const char *utf8) {
    int n = 0;
    for (const uint8_t *p = (const uint8_t *)utf8; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

void ar_utf8_prefix(const char *utf8, int n_cps, char *out, int out_n) {
    int o = 0, seen = 0;
    const uint8_t *p = (const uint8_t *)utf8;
    for (; *p && o + 1 < out_n; p++) {
        if ((*p & 0xC0) != 0x80) { if (seen == n_cps) break; seen++; }
        out[o++] = (char)*p;
    }
    out[o] = '\0';
}

/* ─── Contextual shaping, in place ─── */
static int shape(uint16_t *cp, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        const uint16_t *f = forms_of(cp[i]);
        if (!f) { cp[w++] = cp[i]; continue; }
        int prev_joins = i > 0 && joins_left(cp[i - 1]);
        if (cp[i] == 0x0644 && i + 1 < n) {
            uint16_t iso, fin;
            if (lam_alef(cp[i + 1], &iso, &fin)) {
                cp[w++] = prev_joins ? fin : iso;
                i++;                                   /* alef consumed */
                continue;
            }
        }
        int next_joins = i + 1 < n && joins_right(cp[i + 1]);
        if      (prev_joins && next_joins && f[3]) cp[w++] = f[3];
        else if (prev_joins && f[1])               cp[w++] = f[1];
        else if (next_joins && f[2])               cp[w++] = f[2];
        else                                       cp[w++] = f[0];
    }
    return w;
}

/* ─── Glyph lookup: code point → table index, or -1 ─── */
static int glyph_index(uint16_t cp) {
    if (cp >= 0xFE80 && cp < 0xFE80 + FONT_AR_PRES_COUNT) return FONT_AR_PRES_FIRST_IDX + (cp - 0xFE80);
    if (cp >= 0x0660 && cp <= 0x0669) return FONT_AR_DIGITS_FIRST + (cp - 0x0660);
    switch (cp) {
        case ' ':    return 0;
        case 0x060C: return 5;       /* ، */
        case 0x061F: return 6;       /* ؟ */
        default:     return -1;
    }
}

/* ASCII that the Latin 5×7 font supplies (drawn on the Arabic baseline). */
static int latin_ok(uint16_t cp) {
    return cp < 0x80 && cp != ' ' && font_glyph((char)cp) != NULL;
}

static int glyph_adv(const ar_font_t *f, uint16_t cp) {
    if (cp == ' ') return f->space_w;
    int gi = glyph_index(cp);
    if (gi >= 0) return f->adv[gi];
    if (latin_ok(cp)) return 6;
    return 0;
}

static int g_invert = 0;      /* set by the draw call: 1 = clear pixels (highlight bars) */

static inline void put(int x, int y) { if (g_invert) px_clr(x, y); else px_set(x, y); }

static void blit(const ar_font_t *f, uint16_t cp, int x, int y0) {
    int gi = glyph_index(cp);
    if (gi >= 0) {
        x += f->lb[gi];
        const uint32_t *rows = f->bits + gi * f->rows;
        for (int r = 0; r < f->rows; r++) {
            uint32_t bits = rows[r];
            if (!bits) continue;
            for (int c = 0; c < 32; c++)
                if (bits & (1u << c)) put(x + c, y0 + r);
        }
    } else if (latin_ok(cp)) {
        /* 7-row Latin glyph with its bottom on the Arabic baseline */
        const uint8_t *g = font_glyph((char)cp);
        int ly = y0 + f->baseline - 6;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c)) put(x + c, ly + r);
    }
}

/* A left-to-right run inside the RTL stream: Arabic-Indic digits and ASCII
 * words (numbers, IPs, SSIDs, "C"/"L" key hints). The run starts at an
 * alphanumeric and stops before trailing sentence punctuation, so "٨." keeps
 * its full stop on the reading side. */
static inline int is_ar_digit(uint16_t c) { return c >= 0x0660 && c <= 0x0669; }
static inline int is_ascii_alnum(uint16_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static int ltr_run_len(const uint16_t *g, int i, int n) {
    int starts = is_ar_digit(g[i]) || is_ascii_alnum(g[i]);
    if (!starts && i + 1 < n && is_ascii_alnum(g[i + 1])) {      /* "-61", "~18H", "#1A2B", "(CAPS)" */
        uint16_t c = g[i];
        starts = (c == '-' || c == '+' || c == '~' || c == '#' || c == '(');
    }
    if (!starts) return 0;
    int j = i;
    while (j < n && (is_ar_digit(g[j]) || (g[j] < 0x80 && g[j] != ' '))) j++;
    while (j > i + 1) {                      /* drop trailing . , ! ? : from the run */
        uint16_t c = g[j - 1];
        if (c == '.' || c == ',' || c == '!' || c == '?' || c == ':') j--; else break;
    }
    return j - i;
}

static int word_is_ascii(const uint16_t *g, int s, int e) {
    for (int i = s; i < e; i++) if (g[i] >= 0x80) return 0;
    return e > s;
}

/* Draw shaped glyphs g[0..n) right-to-left ending at x_right; returns width. */
static int draw_run_rtl(const ar_font_t *f, const uint16_t *g, int n, int x_right, int y0, int do_draw) {
    int x = x_right;
    for (int i = 0; i < n; ) {
        int run = ltr_run_len(g, i, n);
        if (run) {
            int w = 0;
            for (int k = i; k < i + run; k++) w += glyph_adv(f, g[k]);
            x -= w;
            int xx = x;
            for (int k = i; k < i + run; k++) { if (do_draw) blit(f, g[k], xx, y0); xx += glyph_adv(f, g[k]); }
            i += run;
            continue;
        }
        x -= glyph_adv(f, g[i]);
        if (do_draw) blit(f, g[i], x, y0);
        i++;
    }
    return x_right - x;
}

/* ─── Public API ─── */
static int prepare(const char *utf8, uint16_t *buf) {
    int n = utf8_decode(utf8, buf, MAX_CPS);
    return shape(buf, n);
}

int text_ar_width_f(const ar_font_t *f, const char *utf8) {
    uint16_t buf[MAX_CPS];
    int n = prepare(utf8, buf);
    int w = 0;
    for (int i = 0; i < n; i++) w += glyph_adv(f, buf[i]);
    return w;
}

int draw_text_ar_f(const ar_font_t *f, int x_right, int y0, const char *utf8,
                   int max_w, int max_lines, int invert) {
    uint16_t buf[MAX_CPS];
    int n = prepare(utf8, buf);
    g_invert = invert;

    /* Split into words (on spaces) with their pixel widths. */
    int ws[MAX_WORDS], we[MAX_WORDS], ww[MAX_WORDS], nw = 0;
    for (int i = 0; i < n && nw < MAX_WORDS; ) {
        while (i < n && buf[i] == ' ') i++;
        if (i >= n) break;
        int s = i, w = 0;
        while (i < n && buf[i] != ' ') { w += glyph_adv(f, buf[i]); i++; }
        ws[nw] = s; we[nw] = i; ww[nw] = w; nw++;
    }

    int lines = 0, y = y0, i = 0, pitch = ar_font_pitch(f);
    while (i < nw && lines < max_lines) {
        /* Greedy fill: words [i, j) fit on this line. */
        int j, w = ww[i];
        for (j = i + 1; j < nw; j++) {
            if (w + f->space_w + ww[j] > max_w) break;
            w += f->space_w + ww[j];
        }
        int x = x_right;
        for (int k = i; k < j; ) {
            if (k > i) x -= f->space_w;
            /* Consecutive ASCII-only words ("Oct 12 2026", "My Home Net") are
             * one left-to-right block, so their word order survives. */
            int g = k;
            while (g < j && word_is_ascii(buf, ws[g], we[g])) g++;
            if (g - k >= 2) {
                int gw = 0;
                for (int m = k; m < g; m++) gw += ww[m] + (m > k ? f->space_w : 0);
                x -= gw;
                int xx = x;
                for (int m = k; m < g; m++) {
                    for (int c = ws[m]; c < we[m]; c++) {     /* plain LTR, punctuation included */
                        blit(f, buf[c], xx, y);
                        xx += glyph_adv(f, buf[c]);
                    }
                    xx += f->space_w;
                }
                k = g;
            } else {
                x -= draw_run_rtl(f, &buf[ws[k]], we[k] - ws[k], x, y, 1);
                k++;
            }
        }
        i = j; lines++; y += pitch;
    }
    g_invert = 0;
    return lines;
}

int draw_text_ar(int x_right, int y0, const char *utf8, int max_w, int max_lines) {
    return draw_text_ar_f(AR_FONT_Q, x_right, y0, utf8, max_w, max_lines, 0);
}

int text_ar_width(const char *utf8) { return text_ar_width_f(AR_FONT_Q, utf8); }

void draw_text_ar_centered(int x0, int w, int y0, const char *utf8) {
    int tw = text_ar_width(utf8);
    int x_right = x0 + (w + tw) / 2;
    if (x_right > x0 + w) x_right = x0 + w;
    draw_text_ar(x_right, y0, utf8, w, 1);
}

int ar_itoa(char *buf, int n, unsigned v) {
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 11);
    int o = 0;
    while (t > 0 && o + 2 < n) {           /* ٠ = D9 A0 .. ٩ = D9 A9 */
        buf[o++] = (char)0xD9;
        buf[o++] = (char)(0xA0 + (tmp[--t] - '0'));
    }
    buf[o] = '\0';
    return o;
}

/* ═══ UI hooks (called from text.c) ═════════════════════════════════════════
 * Mirror rule: a Latin call at x with width max_w becomes an RTL block whose
 * right edge is canvas_w - x. Vertical rule: the Arabic baseline sits on the
 * Latin baseline (y + 6 for the 5×7 font, y + 13 for the 2x "big" font). */

static int has_arabic(const char *s) {
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) if (*p >= 0x80) return 1;
    return 0;
}

/* Strip a menu cursor prefix. Returns 0 none, 1 "> ", 2 ">>", 3 "  " (unselected pad). */
static int strip_prefix(const char **s) {
    if ((*s)[0] == '>' && (*s)[1] == '>') { *s += 2; return 2; }
    if ((*s)[0] == '>' && (*s)[1] == ' ') { *s += 2; return 1; }
    if ((*s)[0] == ' ' && (*s)[1] == ' ') { *s += 2; return 3; }
    return 0;
}

static void cat(char *buf, int n, int *o, const char *s) {
    while (*s && *o < n - 1) buf[(*o)++] = *s++;
    buf[*o] = '\0';
}

/* Translate `s` into buf. Rules, in order: exact table hit; "KEY: VALUE"
 * translated piecewise (either half may hit); "LABEL:" with a known label;
 * "* NAME *" wrappers around a known name. A line is handed to the Arabic
 * layout (mirrored) as soon as ANY part hit, so e.g. "IP: 192.168.1.42"
 * lines up with its translated neighbours. Already-Arabic input passes. */
static int translate(const char *s, char *buf, int n) {
    int o = 0;
    const char *t = i18n_lookup(s);
    if (t) { cat(buf, n, &o, t); return 1; }

    const char *colon = strstr(s, ": ");
    if (colon) {
        char key[48];
        int kl = (int)(colon - s);
        if (kl < (int)sizeof(key)) {
            memcpy(key, s, kl); key[kl] = '\0';
            const char *tk = i18n_lookup(key);
            const char *tv = i18n_lookup(colon + 2);
            if (tk || tv) {
                cat(buf, n, &o, tk ? tk : key);
                cat(buf, n, &o, ": ");
                cat(buf, n, &o, tv ? tv : colon + 2);
                return 1;
            }
        }
    }
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == ':' && len < 48) {              /* "ACCEL:" */
        char key[48]; memcpy(key, s, len - 1); key[len - 1] = '\0';
        if ((t = i18n_lookup(key)) != NULL) { cat(buf, n, &o, t); cat(buf, n, &o, ":"); return 1; }
    }
    if (len > 4 && s[0] == '*' && s[1] == ' ' && s[len - 2] == ' ' && s[len - 1] == '*' && len < 48) {
        char key[48]; memcpy(key, s + 2, len - 4); key[len - 4] = '\0';   /* "* WAVE *" */
        if ((t = i18n_lookup(key)) != NULL) { cat(buf, n, &o, "* "); cat(buf, n, &o, t); cat(buf, n, &o, " *"); return 1; }
    }
    cat(buf, n, &o, s);
    return has_arabic(buf);
}

/* Cursor glyph on the right edge (mirrored '>'): a 4×7 left-pointing chevron. */
static void cursor_glyph(int x, int y, int dbl) {
    for (int r = 0; r < 7; r++) {
        int c = (r < 4) ? 3 - r : r - 3;                /* 3,2,1,0,1,2,3 */
        put(x + c, y + r);
        if (dbl) put(x - 5 + c, y + r);
    }
}

int ar_ui_text(int x, int y, const char *text, int max_w) {
    if (!lang_is_ar()) return 0;
    const char *s = text;
    int pre = strip_prefix(&s);
    char buf[96];
    if (!translate(s, buf, sizeof(buf))) return 0;     /* English-only: Latin path */

    const ar_font_t *f = AR_FONT_UI;
    int x_right = canvas_w - x;
    if (pre == 1 || pre == 2) { cursor_glyph(x_right - 5, y, pre == 2); x_right -= 12; }
    else if (pre == 3)        { x_right -= 12; }
    int avail = max_w < x_right ? max_w : x_right - 1;
    g_invert = 0;
    draw_text_ar_f(f, x_right, y + 6 - f->baseline, buf, avail, 3, 0);
    return 1;
}

int ar_ui_inverted(int y, const char *text) {
    if (!lang_is_ar()) return 0;
    const char *s = text;
    int pre = strip_prefix(&s);
    char buf[96];
    if (!translate(s, buf, sizeof(buf))) return 0;

    const ar_font_t *f = AR_FONT_UI;
    int hx_end = canvas_w - 6;
    for (int hy = y - 2; hy < y + 10; hy++)             /* 12-row bar: body + most descenders */
        for (int hx = 6; hx < hx_end; hx++) px_set(hx, hy);
    int x_right = canvas_w - 10;
    g_invert = 1;
    if (pre == 1 || pre == 2) { cursor_glyph(x_right - 5, y, pre == 2); x_right -= 12; }
    else if (pre == 3)        { x_right -= 12; }
    g_invert = 0;
    draw_text_ar_f(f, x_right, y + 6 - f->baseline, buf, x_right - 8, 1, 1);
    return 1;
}

int ar_ui_big(int x, int y, const char *text, int max_w) {
    if (!lang_is_ar()) return 0;
    char buf[96];
    if (!translate(text, buf, sizeof(buf))) return 0;
    const ar_font_t *f = AR_FONT_BIG;
    int x_right = canvas_w - x;
    int avail = max_w < x_right ? max_w : x_right - 1;
    draw_text_ar_f(f, x_right, y + 13 - f->baseline, buf, avail, 4, 0);
    return 1;
}

int ar_ui_bold(int x, int y, const char *text) {
    if (!lang_is_ar()) return 0;
    char buf[96];
    if (!translate(text, buf, sizeof(buf))) return 0;
    const ar_font_t *f = AR_FONT_UI;
    int x_right = canvas_w - x;
    draw_text_ar_f(f, x_right,     y + 6 - f->baseline, buf, x_right - 1, 1, 0);
    draw_text_ar_f(f, x_right + 1, y + 6 - f->baseline, buf, x_right - 1, 1, 0);
    return 1;
}
