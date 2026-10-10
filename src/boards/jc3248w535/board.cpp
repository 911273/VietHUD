// JC3248W535 (VietHUD 3.5") — core/Board.h implementation.
// Pins/capabilities: include/boards/jc3248w535/board_config.h.
#include "core/Board.h"
#include "boards/board.h"
#include "AXS15231BTouch.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Nothing to bring up early: the NS4168 amp has no enable line and the touch
// driver starts its own I2C bus.
void boardEarlyInit() {}
void boardAmpEnable(bool) {}

// AXS15231B on QSPI. See display/DisplayDriver.h: setRotation() on the raw
// panel and partial-frame writes are both broken on this controller, which is
// why DisplayDriver always wraps it in a full-frame Arduino_Canvas.
Arduino_GFX *boardCreatePanel() {
    Arduino_DataBus *bus = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
    return new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, TFT_RES_W, TFT_RES_H);
}

static AXS15231BTouch touch(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_ADDR);

bool boardTouchBegin() {
    if (!touch.begin()) return false;
    // Raw controller range -> panel pixels; the range is the panel's native
    // geometry regardless of the current logical rotation.
    touch.enableOffsetCorrection(true);
    touch.setOffsets(TOUCH_X_MIN, TOUCH_X_MAX, TFT_RES_W - 1, TOUCH_Y_MIN, TOUCH_Y_MAX, TFT_RES_H - 1);
    return true;
}

// Must numerically match DisplayDriver's canvas rotation — AXS15231BTouch's
// rotation switch uses the identical 0-3 convention.
void boardTouchSetRotation(uint8_t rotation) { touch.setRotation(rotation); }
bool boardTouchRead(uint16_t *x, uint16_t *y) { return touch.getPoint(x, y); }
