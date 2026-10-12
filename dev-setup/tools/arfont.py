#!/usr/bin/env python3
"""arfont.py — build a tiny 1-bit Arabic bitmap font from a system TTF.

Each Arabic Presentation Form-B glyph (U+FE80..U+FEFC), the Arabic-Indic
digits, and the few punctuation marks the quotes use are rasterised one at a
time with FreeType's mono renderer, so the firmware only needs a table lookup
per shaped character (no OpenType shaping on the device).

Usage:
  arfont.py preview FONT.ttf SIZE out.png       # render sample lines
  arfont.py header  out.h FONT.ttf ui:10 q:11 big:16   # emit the C tables
"""
import sys, math
from PIL import Image, ImageFont, ImageDraw

# ── Shaping tables ─────────────────────────────────────────────────────────
# letter -> (isolated, final, initial, medial) presentation-form codepoints;
# None where the letter has no such form (right-joining letters).
FORMS = {
    0x0621: (0xFE80, None,   None,   None),    # ء
    0x0622: (0xFE81, 0xFE82, None,   None),    # آ
    0x0623: (0xFE83, 0xFE84, None,   None),    # أ
    0x0624: (0xFE85, 0xFE86, None,   None),    # ؤ
    0x0625: (0xFE87, 0xFE88, None,   None),    # إ
    0x0626: (0xFE89, 0xFE8A, 0xFE8B, 0xFE8C),  # ئ
    0x0627: (0xFE8D, 0xFE8E, None,   None),    # ا
    0x0628: (0xFE8F, 0xFE90, 0xFE91, 0xFE92),  # ب
    0x0629: (0xFE93, 0xFE94, None,   None),    # ة
    0x062A: (0xFE95, 0xFE96, 0xFE97, 0xFE98),  # ت
    0x062B: (0xFE99, 0xFE9A, 0xFE9B, 0xFE9C),  # ث
    0x062C: (0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0),  # ج
    0x062D: (0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4),  # ح
    0x062E: (0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8),  # خ
    0x062F: (0xFEA9, 0xFEAA, None,   None),    # د
    0x0630: (0xFEAB, 0xFEAC, None,   None),    # ذ
    0x0631: (0xFEAD, 0xFEAE, None,   None),    # ر
    0x0632: (0xFEAF, 0xFEB0, None,   None),    # ز
    0x0633: (0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4),  # س
    0x0634: (0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8),  # ش
    0x0635: (0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC),  # ص
    0x0636: (0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0),  # ض
    0x0637: (0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4),  # ط
    0x0638: (0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8),  # ظ
    0x0639: (0xFEC9, 0xFECA, 0xFECB, 0xFECC),  # ع
    0x063A: (0xFECD, 0xFECE, 0xFECF, 0xFED0),  # غ
    0x0641: (0xFED1, 0xFED2, 0xFED3, 0xFED4),  # ف
    0x0642: (0xFED5, 0xFED6, 0xFED7, 0xFED8),  # ق
    0x0643: (0xFED9, 0xFEDA, 0xFEDB, 0xFEDC),  # ك
    0x0644: (0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0),  # ل
    0x0645: (0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4),  # م
    0x0646: (0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8),  # ن
    0x0647: (0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC),  # ه
    0x0648: (0xFEED, 0xFEEE, None,   None),    # و
    0x0649: (0xFEEF, 0xFEF0, None,   None),    # ى
    0x064A: (0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4),  # ي
}
# lam + alef variants -> (isolated, final)
LAM_ALEF = {0x0622: (0xFEF5, 0xFEF6), 0x0623: (0xFEF7, 0xFEF8),
            0x0625: (0xFEF9, 0xFEFA), 0x0627: (0xFEFB, 0xFEFC)}

def joins_left(cp):   # can connect to the following letter (has initial/medial)
    f = FORMS.get(cp); return bool(f and f[2])
def joins_right(cp):  # can connect to the preceding letter (has a final form)
    f = FORMS.get(cp); return bool(f and f[1])

def shape(text):
    """Logical-order Unicode -> list of presentation-form codepoints (other
    characters pass through unchanged). Mirrors the firmware algorithm."""
    cps = [ord(c) for c in text]
    out = []
    i = 0
    n = len(cps)
    while i < n:
        cp = cps[i]
        if cp not in FORMS:
            out.append(cp); i += 1; continue
        prev_joins = i > 0 and cps[i-1] in FORMS and joins_left(cps[i-1])
        # lam-alef ligature
        if cp == 0x0644 and i + 1 < n and cps[i+1] in LAM_ALEF:
            iso, fin = LAM_ALEF[cps[i+1]]
            out.append(fin if prev_joins else iso)
            i += 2; continue
        next_joins = i + 1 < n and cps[i+1] in FORMS and joins_right(cps[i+1])
        iso, fin, ini, med = FORMS[cp]
        if prev_joins and next_joins and med: out.append(med)
        elif prev_joins and fin:               out.append(fin)
        elif next_joins and ini:               out.append(ini)
        else:                                  out.append(iso)
        i += 1
    return out

# ── Glyph set ──────────────────────────────────────────────────────────────
PUNCT = [0x0020, 0x002E, 0x002D, 0x0021, 0x0025, 0x060C, 0x061F]   # space . - ! % ، ؟
DIGITS = list(range(0x0660, 0x066A))
PRES = list(range(0xFE80, 0xFEFD))
GLYPHS = PUNCT + DIGITS + PRES

def rasterise(ttf, size):
    font = ImageFont.truetype(ttf, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, descent = font.getmetrics()
    H = ascent + descent
    table = {}
    for cp in GLYPHS:
        ch = chr(cp)
        adv = font.getlength(ch)
        mask, (ox, oy) = font.getmask2(ch, mode="1")
        w, h = mask.size
        rows = [0] * H
        lb = min(ox, 0)                     # left overhang → per-glyph bearing
        minx, maxx = 99, -1
        for y in range(h):
            yy = y + oy
            if not (0 <= yy < H): continue
            for x in range(w):
                if mask.getpixel((x, y)):
                    xx = x + ox - lb
                    if xx > 31: continue
                    rows[yy] |= (1 << xx)
                    minx = min(minx, xx); maxx = max(maxx, xx)
        table[cp] = (int(round(adv)), rows, lb, maxx)
    return table, H, ascent

def trim(table, H):
    """Drop empty top/bottom rows shared by every glyph."""
    top = H; bot = -1
    for cp, (adv, rows, _, _) in table.items():
        for y, r in enumerate(rows):
            if r: top = min(top, y); bot = max(bot, y)
    return top, bot

# ── Rendering (host preview; same algorithm as the firmware) ───────────────
def layout_lines(text, table, max_w, space_w):
    words = text.split(' ')
    lines, cur, cur_w = [], [], 0
    for w in words:
        g = shape(w)
        ww = sum(table[c][0] if c in table else 0 for c in g)
        add = ww if not cur else ww + space_w
        if cur and cur_w + add > max_w:
            lines.append(cur); cur, cur_w = [g], ww
        else:
            cur.append(g); cur_w += add
    if cur: lines.append(cur)
    return lines

def draw_rtl(img, x_right, y, line_words, table, space_w, top):
    px = img.load()
    x = x_right
    def blit(cp, x0):
        adv, rows, lb, _ = table[cp]
        x0 += lb
        for yy, r in enumerate(rows):
            if not r: continue
            for xx in range(16):
                if r & (1 << xx) and 0 <= x0 + xx < img.width and 0 <= y + yy - top < img.height:
                    px[x0 + xx, y + yy - top] = 0
    for wi, g in enumerate(line_words):
        if wi: x -= space_w
        i = 0
        while i < len(g):
            cp = g[i]
            if 0x0660 <= cp <= 0x0669:          # digit run: LTR inside RTL
                j = i
                while j < len(g) and 0x0660 <= g[j] <= 0x0669: j += 1
                run_w = sum(table[c][0] for c in g[i:j])
                x -= run_w
                xx = x
                for c in g[i:j]:
                    blit(c, xx); xx += table[c][0]
                i = j; continue
            if cp in table:
                x -= table[cp][0]; blit(cp, x)
            i += 1

SAMPLES = [
    "الواي فاي ده هوا حار بس.",
    "عندي ٨ دراعات وصفر صبر خالص!",
    "أنا ٩٠% مية. يعني أنا رشة بس.",
    "السمك ده عصافير مبلولة، صح؟",
    "مش هقول حاجة. جريج بيفكر.",
    "لسه فاكر البحر؟ كان هادي أوي وقتها.",
    "طب إيه رأيك نعمل كشري دلوقتي؟ عشان أنا جعان.",
]

def preview(ttf, size, out):
    table, H, ascent = rasterise(ttf, size)
    top, bot = trim(table, H)
    pitch = bot - top + 2
    space_w = table[0x20][0]
    W = 158
    img = Image.new("1", (W + 8, (len(SAMPLES) * 3) * pitch + 10), 1)
    y = 4
    for s in SAMPLES:
        for lw in layout_lines(s, table, W, space_w):
            draw_rtl(img, W + 4, y, lw, table, space_w, top)
            y += pitch
        y += 2
    img = img.resize((img.width * 4, img.height * 4), Image.NEAREST)
    img.save(out)
    print(f"{ttf} size={size}: H={H} rows {top}..{bot} pitch={pitch} space={space_w} "
          f"avg_adv={sum(t[0] for t in table.values())/len(table):.1f}")

def header(out, ttf, specs):
    """specs: list of (name, size). One header holds every font; glyph order is
    PUNCT + DIGITS + PRES for all of them (the firmware shares the index map)."""
    with open(out, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by dev-setup/tools/arfont.py — do not edit by hand.\n")
        f.write(f" * Source: {ttf.split('/')[-1]}, FreeType mono rendering.\n")
        f.write(" * Glyph order (all fonts): " + " ".join(f"U+{c:04X}" for c in PUNCT)
                + " | U+0660..0669 | U+FE80..FEFC\n")
        f.write(" * bits[idx*rows + r]: bit x of row r = pixel (x, r), x grows LEFT→RIGHT. */\n")
        f.write("#ifndef FONT_AR_H\n#define FONT_AR_H\n#include <stdint.h>\n\n")
        f.write(f"#define FONT_AR_PUNCT_COUNT {len(PUNCT)}\n#define FONT_AR_DIGITS_FIRST {len(PUNCT)}\n")
        f.write(f"#define FONT_AR_PRES_FIRST_IDX {len(PUNCT)+len(DIGITS)}\n#define FONT_AR_PRES_COUNT {len(PRES)}\n")
        f.write(f"#define FONT_AR_GLYPHS {len(GLYPHS)}\n\n")
        f.write("typedef struct ar_font {\n    uint8_t rows, baseline, space_w, maxw;\n"
                "    const uint8_t  *adv;   /* [FONT_AR_GLYPHS] */\n"
                "    const int8_t   *lb;    /* [FONT_AR_GLYPHS] left bearing (<= 0) */\n"
                "    const uint32_t *bits;  /* [FONT_AR_GLYPHS * rows] */\n} ar_font_t;\n\n")
        for name, size in specs:
            table, H, ascent = rasterise(ttf, size)
            top, bot = trim(table, H)
            rows_n = bot - top + 1
            maxw = max(t[0] for t in table.values())
            assert maxw <= 32, maxw
            f.write(f"/* ── {name}: {size} px, {rows_n} rows, baseline row {ascent-top} ── */\n")
            f.write(f"static const uint8_t {name}_adv[FONT_AR_GLYPHS] = {{\n    "
                    + ",".join(str(table[cp][0]) for cp in GLYPHS) + "\n};\n")
            f.write(f"static const int8_t {name}_lb[FONT_AR_GLYPHS] = {{\n    "
                    + ",".join(str(table[cp][2]) for cp in GLYPHS) + "\n};\n")
            f.write(f"static const uint32_t {name}_bits[FONT_AR_GLYPHS * {rows_n}] = {{\n")
            for cp in GLYPHS:
                adv, rows, lb, _ = table[cp]
                r = ",".join(f"0x{v:x}" for v in rows[top:bot+1])
                f.write(f"    {r}, /* U+{cp:04X} {chr(cp) if cp != 0x20 else 'space'} */\n")
            f.write("};\n")
            f.write(f"static const ar_font_t {name} = {{ {rows_n}, {ascent-top}, {table[0x20][0]}, {maxw}, "
                    f"{name}_adv, {name}_lb, {name}_bits }};\n\n")
            print(f"{name}: {size}px rows={rows_n} baseline={ascent-top} space={table[0x20][0]} maxw={maxw}")
        f.write("#endif /* FONT_AR_H */\n")
    print(f"wrote {out}")

if __name__ == "__main__":
    if sys.argv[1] == "preview":
        preview(sys.argv[2], int(sys.argv[3]), sys.argv[4])
    elif sys.argv[1] == "header":
        # arfont.py header out.h FONT.ttf name:size [name:size ...]
        specs = [(a.split(":")[0], int(a.split(":")[1])) for a in sys.argv[4:]]
        header(sys.argv[2], sys.argv[3], specs)
    else:
        print(__doc__)
