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

void dispFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    uint32_t t0 = micros();
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h); // into canvas RAM
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
