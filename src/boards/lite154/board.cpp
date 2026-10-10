// VietHUD Lite 1.54" — core/Board.h implementation.
// Pins/capabilities: include/boards/lite154/board_config.h.
#include "core/Board.h"
#include "boards/board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

void boardEarlyInit() {
    // The board latches its own power: GPIO21 must go high or it switches
    // itself off as soon as the power button is released. Done first.
    pinMode(PIN_POWER_HOLD, OUTPUT);
    digitalWrite(PIN_POWER_HOLD, HIGH);
    pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
    pinMode(BTN_CENTER_PIN, INPUT_PULLUP);
    pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
    Serial.println("[board] VietHUD Lite 1.54 (ST7789 240x240, buttons, on-chip data)");
}

void boardAmpEnable(bool) {} // amp has no enable line

// ST7789 240x240 IPS on SPI, the same parameters the stand-alone Lite firmware
// used (IPS inversion on, no column/row offset).
Arduino_GFX *boardCreatePanel() {
    Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
    return new Arduino_ST7789(bus, TFT_RST, 0, true, TFT_RES_W, TFT_RES_H, 0, 0, 0, 0);
}

// No touch controller.
bool boardTouchBegin() { return true; }
void boardTouchSetRotation(uint8_t) {}
bool boardTouchRead(uint16_t *, uint16_t *) { return false; }

uint8_t boardButtonsRaw() {
    uint8_t m = 0;
    if (digitalRead(BTN_LEFT_PIN) == LOW) m |= BOARD_KEY_LEFT;
    if (digitalRead(BTN_CENTER_PIN) == LOW) m |= BOARD_KEY_CENTER;
    if (digitalRead(BTN_RIGHT_PIN) == LOW) m |= BOARD_KEY_RIGHT;
    return m;
}
