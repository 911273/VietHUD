#include "DisplayDriver.h"
#include "core/AppConfig.h"
#include "pincfg.h"
#include "dispcfg.h"

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
static Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, TFT_RES_W, TFT_RES_H);
// Constructed inside displayBegin(), NOT here at file-scope static-init
// time — cfg.screenRotation isn't loaded from NVS until
// loadConfigFromNVS(cfg) runs partway through main_ui_demo.cpp's setup(),
// and static-init order across translation units (this file's `gfx` vs.
// main_ui_demo.cpp's `cfg`) is unspecified in C++, so reading cfg here
// directly would be a real, if intermittent-looking, ordering bug.
Arduino_Canvas *gfx = nullptr;

uint32_t g_flushUs = 0, g_flushCount = 0, g_renderUs = 0;

// Cache-blocked replacement for Arduino_Canvas::draw16bitRGBBitmap on a
// rotation-1 canvas. WHY (2026-09-24 perf P0): the library's rotate_1 path
// writes each LVGL landscape row DOWN a framebuffer column (stride =
// canvas height, 320 px = 640 B per step). The 307 KB canvas framebuffer
// lives in PSRAM (only ~47 KB internal SRAM is free — it cannot fit there),
// and a 640 B stride misses the PSRAM cache line on EVERY pixel: a full-
// screen redraw (the heading-up map rotates every frame) measured 141 ms
// just for this copy, capping the UI at ~5 FPS.
//
// This version keeps the transpose but flips which side scatters. For a
// rotation-1 canvas, landscape pixel (X,Y) maps to framebuffer index
// X*stride + (maxY - Y) with stride = gfx->height(), maxY = stride-1 (this
// is exactly Arduino_Canvas::writePixelPreclipped case 1). Iterating Y
// DOWNWARD for a fixed X hits CONSECUTIVE framebuffer addresses, so the
// PSRAM writes become long contiguous bursts instead of per-pixel misses.
// The compensating strided access lands on px_map instead — and px_map is
// LVGL's draw buffer in INTERNAL SRAM, where a stride costs nothing. Net:
// the 141 ms scatter dropped to a bandwidth-bound copy. Byte layout is
// identical to the library path (raw uint16 store), so colour is unchanged.
static inline void blitRot1ToCanvas(uint16_t *fb, const uint16_t *src, int x1, int y1, int w, int h,
                                    int stride) {
    const int maxY = stride - 1;            // canvas _max_y (portrait height - 1)
    const int yTop = y1 + h - 1;            // largest Y → smallest (maxY - Y) → run start
    for (int xi = 0; xi < w; ++xi) {
        uint16_t *d = fb + (int32_t)(x1 + xi) * stride + (maxY - yTop); // contiguous PSRAM run
        const uint16_t *s = src + (int32_t)(h - 1) * w + xi;            // strided internal-SRAM read
        for (int k = 0; k < h; ++k) {
            *d++ = *s;
            s -= w;
        }
    }
}

// NOTE (2026-09-24 V2): a dirty-row PARTIAL flush was tried here to cut the
// ~33 ms full-frame QSPI push, but on this AXS15231B panel pushing sub-regions
// per frame caused visible flicker/tearing and an apparent left shift, so it
// was reverted to the proven full-frame flush below. The real per-frame cost
// was NOT the flush anyway — it was the whole-screen redraw every tick, which
// is fixed separately (removed the per-tick mapCanvas style-opa invalidation +
// gated the per-tick colour/icon writes in Dashboard.cpp). With those, an idle
// screen dirties nothing, so lv_display flushes nothing (0 ms); only a genuine
// change triggers one full ~33 ms push. blitRot1ToCanvas() still gives the
// cache-friendly transpose (canvas-copy 141 ms -> ~3 ms).
void dispFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    uint32_t t0 = micros();
    if (gfx->getRotation() == 1) {
        blitRot1ToCanvas(gfx->getFramebuffer(), (const uint16_t *)px_map, area->x1, area->y1, (int)w, (int)h,
                         gfx->height()); // fast path: cache-friendly transpose
    } else {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h); // into canvas RAM
    }
    g_renderUs += micros() - t0;

    if (lv_display_flush_is_last(disp)) {
        uint32_t t1 = micros();
        gfx->flush();
        g_flushUs += micros() - t1;
        g_flushCount++;
    }
    lv_display_flush_ready(disp);
}

void displayBegin() {
    // cfg.screenRotation must already be loaded from NVS by the time this
    // runs (main_ui_demo.cpp's setup() calls loadConfigFromNVS(cfg) before
    // displayBegin() — this order is load-bearing, not incidental).
    gfx = new Arduino_Canvas(TFT_RES_W, TFT_RES_H, panel, 0, 0, (uint8_t)cfg.screenRotation);
    if (!gfx->begin()) Serial.println("[display] ERROR: display init failed");
    gfx->fillScreen(RGB565_BLACK); // "BLACK" alias was removed in GFX 1.6.x
    Serial.printf("[display] Canvas size after rotation=%d: %dx%d\n", (int)cfg.screenRotation, gfx->width(),
                  gfx->height());
}

#define BL_LEDC_CHANNEL 0
#define BL_LEDC_FREQ_HZ 5000
#define BL_LEDC_RES_BITS 8

void backlightBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL, BL_LEDC_FREQ_HZ, BL_LEDC_RES_BITS);
#else
    ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ_HZ, BL_LEDC_RES_BITS);
    ledcAttachPin(TFT_BL, BL_LEDC_CHANNEL);
#endif
}

void backlightWrite(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL, duty);
#else
    ledcWrite(BL_LEDC_CHANNEL, duty);
#endif
}
