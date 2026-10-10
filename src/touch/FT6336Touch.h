#pragma once
#include <stdint.h>

// Minimal FT6336G (FocalTech FT6x36) capacitive touch reader for VietHUD 2.8
// (ES3C28P). Same surface as AXS15231BTouch so TouchTask.cpp can use either:
// begin() / setRotation() / getPoint(). Reports the first touch point mapped
// from the panel's native 240x320 portrait frame into the logical frame of
// the given rotation, using Arduino_GFX's rotation convention (so it lines up
// with the Arduino_Canvas the display draws through).
//
// Expects Wire to be started already (boardEarlyInit() owns the bus — the
// ES8311 codec sits on it too) and the controller to be out of reset.
class FT6336Touch {
public:
    FT6336Touch(uint8_t addr, uint16_t nativeW, uint16_t nativeH) : _addr(addr), _w(nativeW), _h(nativeH) {}
    bool begin();
    void setRotation(uint8_t r) { _rot = r & 3; }
    bool getPoint(uint16_t *x, uint16_t *y);

private:
    bool readRaw(uint16_t *x, uint16_t *y);
    uint8_t _addr, _rot = 0;
    uint16_t _w, _h;
    uint16_t _lastX = 0, _lastY = 0;
    uint32_t _lastSeenMs = 0;
    bool _down = false;
};
