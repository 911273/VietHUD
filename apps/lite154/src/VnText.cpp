#include "VnText.h"

namespace {

// Next code point from a UTF-8 string; advances p. Invalid bytes come back as '?'.
uint32_t nextCodepoint(const char *&p) {
    uint8_t c = (uint8_t)*p++;
    if (c < 0x80) return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
    if (extra < 0) return '?';
    uint32_t cp = c & (0x3F >> extra);
    for (int i = 0; i < extra; i++) {
        uint8_t b = (uint8_t)*p;
        if ((b & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (b & 0x3F);
        p++;
    }
    return cp;
}

const VnGlyph *findGlyph(const VnFont &font, uint32_t cp) {
    int lo = 0, hi = font.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t code = font.glyphs[mid].code;
        if (code == cp) return &font.glyphs[mid];
        if (code < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a /* 0..15 */) {
    if (a == 15) return fg;
    uint32_t inv = 15 - a;
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv) / 15;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * inv) / 15;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * inv) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

} // namespace

namespace VnText {

int width(const VnFont &font, const char *text) {
    int w = 0;
    const char *p = text;
    while (*p) {
        const VnGlyph *g = findGlyph(font, nextCodepoint(p));
        if (g) w += g->adv;
    }
    return w;
}

int draw(Arduino_Canvas *canvas, const VnFont &font, int x, int baseline, const char *text,
         uint16_t color, int clipX0, int clipX1) {
    uint16_t *fb = canvas->getFramebuffer();
    const int W = canvas->width();
    const int H = canvas->height();
    if (clipX0 < 0) clipX0 = 0;
    if (clipX1 > W) clipX1 = W;

    const char *p = text;
    while (*p) {
        const VnGlyph *g = findGlyph(font, nextCodepoint(p));
        if (!g) continue;
        int gx = x + g->xo;
        int gy = baseline + g->yo;
        if (fb && g->w && gx + g->w > clipX0 && gx < clipX1) {
            const uint8_t *bits = font.bitmap + g->off;
            uint32_t n = 0;
            for (int row = 0; row < g->h; row++) {
                int py = gy + row;
                for (int col = 0; col < g->w; col++, n++) {
                    uint8_t byte = bits[n >> 1];
                    uint8_t a = (n & 1) ? (byte & 0x0F) : (byte >> 4);
                    int px = gx + col;
                    if (!a || px < clipX0 || px >= clipX1 || py < 0 || py >= H) continue;
                    uint16_t &dst = fb[py * W + px];
                    dst = blend565(dst, color, a);
                }
            }
        }
        x += g->adv;
    }
    return x;
}

void drawCentered(Arduino_Canvas *canvas, const VnFont &font, int cx, int baseline, const char *text,
                  uint16_t color) {
    draw(canvas, font, cx - width(font, text) / 2, baseline, text, color);
}

void drawRight(Arduino_Canvas *canvas, const VnFont &font, int xRight, int baseline, const char *text,
               uint16_t color) {
    draw(canvas, font, xRight - width(font, text), baseline, text, color);
}

} // namespace VnText
