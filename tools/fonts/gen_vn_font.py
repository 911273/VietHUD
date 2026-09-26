# -*- coding: utf-8 -*-
"""
gen_vn_font.py - Generate an LVGL v9 C font with the FULL Vietnamese repertoire.

Why this exists (2026-09-24): the dashboard's only text font was Montserrat,
which has NO Vietnamese diacritics, so every road name (stored UTF-8 in
names.bin) and every hard-coded VN label rendered as tofu/ASCII. This machine
has no Node (so lv_font_conv is unavailable) and no fonttools/freetype-py, but
it DOES have Pillow (with FreeType). So we rasterize each glyph with Pillow and
emit the exact LVGL 9.2.2 "fmt_txt" C structure by hand -- same approach the
existing lv_font_montserrat_speed.c used.

Format constraints (verified against this project's pinned lvgl 9.2.2):
  * bpp MUST be 1/2/4 -- lv_font_fmt_txt.c's PLAIN decoder does not implement
    bpp=8 (it silently renders garbage). We use bpp=4 (16 grey levels).
  * bitmap is a continuous 4-bit nibble stream, high nibble = first pixel,
    box_w*box_h pixels total, 2 px/byte (rows are NOT byte-aligned).
  * cmaps: FORMAT0_TINY for the contiguous ASCII range, SPARSE_TINY for the
    scattered Vietnamese code points (unicode_list holds sorted relative code
    points; glyph_id = glyph_id_start + index-in-list).
  * adv_w is in 1/16 px.  line_height = ascent+descent, base_line = descent.

Re-run to change size/subset. Output is checked in; do not hand-edit the .c.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

# --- config ---------------------------------------------------------------
TTF   = r"C:\Windows\Fonts\arial.ttf"   # Arial ships Vietnamese and reads well small
PPEM  = int(sys.argv[1]) if len(sys.argv) > 1 else 14  # 14 = drop-in for montserrat_14; e.g. 20 for the top bar
BPP   = 4
NAME  = "lv_font_vn_%d" % PPEM
# Fallback font for glyphs we don't carry -- crucially the LVGL FontAwesome
# SYMBOLS (LV_SYMBOL_GPS U+F124, LV_SYMBOL_SETTINGS U+F013, LV_SYMBOL_RIGHT,
# wifi, etc.) that live in the built-in Montserrat. Without this, making our
# font the default made every icon a "glyph not found" (screen looked crashed).
# LVGL 9's lv_font_get_glyph_dsc() walks .fallback recursively.
FALLBACK = "&lv_font_montserrat_%d" % PPEM   # same-size Montserrat (must be enabled in lv_conf.h), or "NULL"
OUT   = os.path.join(os.path.dirname(__file__), "..", "..", "src", "ui", "fonts", NAME + ".c")

# ASCII printable, contiguous 0x20..0x7E
ASCII = [chr(c) for c in range(0x20, 0x7F)]

# Full Vietnamese precomposed repertoire (upper + lower), incl. Đ/đ.
VN_STR = (
    "\u00c0\u00c1\u00c2\u00c3\u00c8\u00c9\u00ca\u00cc\u00cd\u00d2\u00d3\u00d4\u00d5\u00d9\u00da\u00dd"
    "\u00e0\u00e1\u00e2\u00e3\u00e8\u00e9\u00ea\u00ec\u00ed\u00f2\u00f3\u00f4\u00f5\u00f9\u00fa\u00fd"
    "\u0102\u0103\u0110\u0111\u0128\u0129\u0168\u0169\u01a0\u01a1\u01af\u01b0"
    "\u1ea0\u1ea1\u1ea2\u1ea3\u1ea4\u1ea5\u1ea6\u1ea7\u1ea8\u1ea9\u1eaa\u1eab\u1eac\u1ead\u1eae\u1eaf"
    "\u1eb0\u1eb1\u1eb2\u1eb3\u1eb4\u1eb5\u1eb6\u1eb7\u1eb8\u1eb9\u1eba\u1ebb\u1ebc\u1ebd\u1ebe\u1ebf"
    "\u1ec0\u1ec1\u1ec2\u1ec3\u1ec4\u1ec5\u1ec6\u1ec7\u1ec8\u1ec9\u1eca\u1ecb\u1ecc\u1ecd\u1ece\u1ecf"
    "\u1ed0\u1ed1\u1ed2\u1ed3\u1ed4\u1ed5\u1ed6\u1ed7\u1ed8\u1ed9\u1eda\u1edb\u1edc\u1edd\u1ede\u1edf"
    "\u1ee0\u1ee1\u1ee2\u1ee3\u1ee4\u1ee5\u1ee6\u1ee7\u1ee8\u1ee9\u1eea\u1eeb\u1eec\u1eed\u1eee\u1eef"
    "\u1ef0\u1ef1\u1ef2\u1ef3\u1ef4\u1ef5\u1ef6\u1ef7\u1ef8\u1ef9"
)
VN = sorted(set(VN_STR), key=lambda c: ord(c))

font = ImageFont.truetype(TTF, PPEM)
ascent, descent = font.getmetrics()
line_height = ascent + descent
pad = PPEM + 4

def render(ch):
    """Return (box_w, box_h, ofs_x, ofs_y, adv_w, nibble_list) for one char."""
    adv = font.getlength(ch)
    adv_w = int(round(adv * 16))
    cw = int(adv) + 2 * pad
    chh = line_height + 2 * pad
    img = Image.new("L", (cw, chh), 0)
    d = ImageDraw.Draw(img)
    base_y = pad + ascent
    d.text((pad, base_y), ch, font=font, fill=255, anchor="ls")  # ls = left/baseline origin
    bbox = img.getbbox()
    if bbox is None:  # blank glyph (space)
        return 0, 0, 0, 0, adv_w, []
    l, t, r, b = bbox
    box_w, box_h = r - l, b - t
    ofs_x = l - pad
    ofs_y = base_y - b          # baseline to bitmap bottom, +ve = above baseline
    crop = img.crop(bbox)
    px = list(crop.getdata())
    nibs = [(v * 15 + 127) // 255 for v in px]  # 8-bit grey -> 4-bit, rounded
    return box_w, box_h, ofs_x, ofs_y, adv_w, nibs

chars = ASCII + VN
glyphs = []          # (ch, box_w, box_h, ofs_x, ofs_y, adv_w, nibs)
for ch in chars:
    glyphs.append((ch,) + render(ch))

# --- pack bitmaps + build glyph_dsc ---------------------------------------
bitmap_bytes = []
dsc = [(0, 0, 0, 0, 0, 0)]  # id 0 reserved
for (ch, bw, bh, ox, oy, aw, nibs) in glyphs:
    idx = len(bitmap_bytes)
    # pack nibbles, high nibble first, pad to whole byte
    i = 0
    while i < len(nibs):
        hi = nibs[i] & 0xF
        lo = (nibs[i + 1] & 0xF) if i + 1 < len(nibs) else 0
        bitmap_bytes.append((hi << 4) | lo)
        i += 2
    dsc.append((idx, aw, bw, bh, ox, oy))

# --- emit C ---------------------------------------------------------------
def cp(ch):
    return ord(ch)

lines = []
w = lines.append
w("/******************************************************************************")
w(" * LVGL v9 font: ASCII (0x20-0x7E) + full Vietnamese repertoire")
w(" * Size: %d px  Bpp: %d  Source: %s" % (PPEM, BPP, os.path.basename(TTF)))
w(" *")
w(" * GENERATED by tools/fonts/gen_vn_font.py -- do NOT hand-edit. Re-run the")
w(" * generator to change size/subset. bpp=4 PLAIN, TINY(ascii)+SPARSE(vn) cmaps")
w(" * (the only combo this project's pinned lvgl 9.2.2 decoder renders correctly).")
w(" ******************************************************************************/")
w("")
w("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
w('    #include "lvgl.h"')
w("#else")
w('    #include "lvgl/lvgl.h"')
w("#endif")
w("")
w("#ifndef %s" % NAME.upper())
w("#define %s 1" % NAME.upper())
w("#endif")
w("")
w("#if %s" % NAME.upper())
w("")
w("/*-----------------\n *    BITMAPS\n *----------------*/")
w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
row = []
for i, b in enumerate(bitmap_bytes):
    row.append("0x%02x," % b)
    if len(row) == 16:
        w("    " + " ".join(row)); row = []
if row:
    w("    " + " ".join(row))
w("};")
w("")
w("/*---------------------\n *  GLYPH DESCRIPTION\n *--------------------*/")
w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
for (bi, aw, bw, bh, ox, oy) in dsc:
    w("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d},"
      % (bi, aw, bw, bh, ox, oy))
w("};")
w("")

# sparse unicode list (relative to VN range start), sorted
vn_start = cp(VN[0])
vn_end = cp(VN[-1])
vn_range_len = vn_end - vn_start + 1
w("/*---------------------\n *  CHARACTER MAPPING\n *--------------------*/")
w("static const uint16_t unicode_list_vn[] = {")
rl = []
for ch in VN:
    rl.append("%d," % (cp(ch) - vn_start))
    if len(rl) == 12:
        w("    " + " ".join(rl)); rl = []
if rl:
    w("    " + " ".join(rl))
w("};")
w("")
w("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
w("    {")
w("        .range_start = 0x20, .range_length = %d, .glyph_id_start = 1," % len(ASCII))
w("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
w("    },")
w("    {")
w("        .range_start = %d, .range_length = %d, .glyph_id_start = %d," % (vn_start, vn_range_len, 1 + len(ASCII)))
w("        .unicode_list = unicode_list_vn, .glyph_id_ofs_list = NULL, .list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY" % len(VN))
w("    }")
w("};")
w("")
w("/*--------------------\n *  ALL CUSTOM DATA\n *--------------------*/")
w("static const lv_font_fmt_txt_dsc_t font_dsc = {")
w("    .glyph_bitmap = glyph_bitmap,")
w("    .glyph_dsc = glyph_dsc,")
w("    .cmaps = cmaps,")
w("    .kern_dsc = NULL,")
w("    .kern_scale = 0,")
w("    .cmap_num = 2,")
w("    .bpp = %d," % BPP)
w("    .kern_classes = 0,")
w("    .bitmap_format = 0,")
w("};")
w("")
w("/*-----------------\n *  PUBLIC FONT\n *----------------*/")
w("const lv_font_t %s = {" % NAME)
w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
w("    .line_height = %d," % line_height)
w("    .base_line = %d," % descent)
w("    .subpx = LV_FONT_SUBPX_NONE,")
w("    .underline_position = 0,")
w("    .underline_thickness = 0,")
w("    .dsc = &font_dsc,")
if FALLBACK and FALLBACK != "NULL":
    w("    .fallback = %s,  /* symbols + any missing glyph fall back here */" % FALLBACK)
w("};")
w("")
w("#endif /* %s */" % NAME.upper())
w("")

with open(os.path.abspath(OUT), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("Wrote", os.path.abspath(OUT))
print("  glyphs: %d ascii + %d vn = %d, bitmap %d bytes, line_height %d, descent %d"
      % (len(ASCII), len(VN), len(chars), len(bitmap_bytes), line_height, descent))
print("  vn range: U+%04X..U+%04X (len %d)" % (vn_start, vn_end, vn_range_len))
