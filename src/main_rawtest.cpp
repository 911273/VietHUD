// Diagnostic-only build: raw Arduino_GFX test, no LVGL at all.
// Used to isolate whether "screen shows nothing" is a base display/backlight/
// reset problem, or specific to the LVGL flush/buffer wiring in main.cpp.
// Build with: pio run -e rawtest -t upload

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#include "pincfg.h"
#include "dispcfg.h"
#include "AXS15231BTouch.h"

Arduino_DataBus *bus = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
Arduino_GFX *gfx = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, TFT_RES_W, TFT_RES_H);

AXS15231BTouch touch(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_ADDR);

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[rawtest] starting...");

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    Serial.println("[rawtest] backlight pin set HIGH");

    bool ok = gfx->begin();
    Serial.printf("[rawtest] gfx->begin() = %d\n", ok);
    Serial.printf("[rawtest] native size: %dx%d\n", gfx->width(), gfx->height());

    gfx->fillScreen(RED);
    delay(800);
    gfx->fillScreen(GREEN);
    delay(800);
    gfx->fillScreen(BLUE);
    delay(800);
    gfx->fillScreen(WHITE);

    gfx->setCursor(20, 140);
    gfx->setTextSize(3);
    gfx->setTextColor(BLACK);
    gfx->println("Hello World!");
    Serial.println("[rawtest] fillScreen + text done, should be visible now");

    bool touchOk = touch.begin();
    Serial.printf("[rawtest] touch.begin() = %d\n", touchOk);
    touch.setRotation(TFT_ROTATION);
    touch.enableOffsetCorrection(true);
    touch.setOffsets(TOUCH_X_MIN, TOUCH_X_MAX, TFT_RES_W - 1, TOUCH_Y_MIN, TOUCH_Y_MAX, TFT_RES_H - 1);
}

void loop() {
    uint16_t x, y;
    if (touch.getPoint(&x, &y)) {
        Serial.printf("[rawtest] touch x=%d y=%d\n", x, y);
        gfx->fillCircle(x, y, 6, RED);
    }
    delay(20);
}
