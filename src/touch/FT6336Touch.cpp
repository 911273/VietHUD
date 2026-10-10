#include "FT6336Touch.h"
#include <Arduino.h>
#include <Wire.h>

static const uint8_t kRegTdStatus = 0x02; // number of touch points, then P1 XH XL YH YL
static const uint8_t kRegChipId = 0xA3;   // 0x36 (FT6236U) / 0x64 (FT6336U) / 0x06 (FT6206)

bool FT6336Touch::begin() {
    Wire.beginTransmission(_addr);
    Wire.write(kRegChipId);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(_addr, (uint8_t)1) != 1) return false;
    Serial.printf("[touch] FT6336 chip id 0x%02X\n", Wire.read());
    return true;
}

// A finger held still was reported as a burst of separate taps on the real
// unit (2026-10-10), so the 1 s hold-to-open-Settings never fired: the chip
// intermittently reports no point (and a read can fail) mid-hold. Only report
// a release once nothing has been seen for kReleaseMs; until then keep
// returning the last point. Presence is judged by TD_STATUS alone — the
// per-point event flag isn't reliable on this part while the finger is still.
static const uint32_t kReleaseMs = 40;

bool FT6336Touch::getPoint(uint16_t *x, uint16_t *y) {
    uint16_t rx, ry;
    uint32_t now = millis();
    if (readRaw(&rx, &ry)) {
        _lastX = rx;
        _lastY = ry;
        _lastSeenMs = now;
        _down = true;
    } else if (_down && now - _lastSeenMs >= kReleaseMs) {
        _down = false;
    }
    if (!_down) return false;
    *x = _lastX;
    *y = _lastY;
    return true;
}

bool FT6336Touch::readRaw(uint16_t *x, uint16_t *y) {
    uint8_t d[5];
    Wire.beginTransmission(_addr);
    Wire.write(kRegTdStatus);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(_addr, (uint8_t)5) != 5) return false;
    for (int i = 0; i < 5; i++) d[i] = Wire.read();

    uint8_t n = d[0] & 0x0F;
    if (n == 0 || n > 2) return false; // 0x0F etc. = no valid touch

    int nx = ((d[1] & 0x0F) << 8) | d[2];
    int ny = ((d[3] & 0x0F) << 8) | d[4];
    if (nx >= _w) nx = _w - 1;
    if (ny >= _h) ny = _h - 1;

    // Native portrait -> logical, matching Arduino_GFX/Arduino_Canvas rotation.
    switch (_rot) {
    case 0: *x = nx; *y = ny; break;
    case 1: *x = ny; *y = _w - 1 - nx; break;
    case 2: *x = _w - 1 - nx; *y = _h - 1 - ny; break;
    default: *x = _h - 1 - ny; *y = nx; break;
    }
    return true;
}
