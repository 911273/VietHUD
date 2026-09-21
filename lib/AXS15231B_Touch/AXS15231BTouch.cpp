#include "AXS15231BTouch.h"

bool AXS15231BTouch::begin() {
    pinMode(_intPin, INPUT);
    bool ok = Wire.begin(_sda, _scl);
    // REVERTED to 100kHz standard mode 2026-09-14. 400kHz was tried to fix
    // "thao tac van rat cham", but real hardware testing then showed the
    // controller chronically getting stuck re-reporting one frozen value
    // (confirmed via serial log: happened continuously, even with no finger
    // on the screen at all, not just under drag load) — reliability lost to
    // this is a much worse trade than the speed gained. 100kHz + a 20ms
    // (not 10ms — see lv_indev_set_period below) poll period is the
    // conservative baseline to re-validate stability from before trying to
    // speed it back up again.
    Wire.setClock(100000);
    // NOTE: this does NOT actually bound a stuck transaction on this core —
    // 50ms is already core 2.x's documented default, and a full firmware
    // freeze still reproduced on real hardware with it in effect (a held
    // drag in Settings). Kept for clarity/in case a future core fixes the
    // underlying driver bug, but do not rely on it: real recovery is
    // getPoint()'s endTransmission()/requestFrom() error counting + bus
    // reset, backed by the Task Watchdog Timer (src/main_ui_demo.cpp) for
    // the case where a transaction blocks outright and never returns at all.
    Wire.setTimeOut(50);
    return ok;
}

void AXS15231BTouch::resetBus() {
    Wire.end();
    delay(2);
    Wire.begin(_sda, _scl);
    // 100kHz, matching begin() — NOT 400kHz. Confirmed on real hardware
    // 2026-09-14 (this file's begin() comment): 400kHz makes this exact
    // controller chronically wedge into repeating one frozen raw value,
    // which is precisely the "stuck" condition below that calls resetBus()
    // in the first place. This function used to reset to 400kHz anyway — a
    // leftover from before that finding, never updated alongside begin()'s
    // own revert. Confirmed as a real, reproducible bug 2026-09-14 during
    // Dashboard.cpp's new 3s WiFi-toggle hold gesture: a genuinely held
    // touch is long enough to make SOME stuck/garbage read (rawX=rawY=2859,
    // the same known-garbage pattern getPoint() already documents seeing
    // under other timing pressure) statistically likely at some point, and
    // once resetBus() bumped the clock to 400kHz, the controller then
    // wedged immediately and permanently at that speed — every subsequent
    // read repeated, re-triggering the stuck check and resetBus() again in
    // a tight, self-sustaining ~500ms loop that a live serial capture
    // showed continuing for 100+ seconds with no finger anywhere near the
    // screen. WiFi wasn't the underlying cause: it just made a 3s+ still
    // hold a normal gesture for the first time, which was enough for this
    // latent bug to surface.
    Wire.setClock(100000);
    _consecutiveErrors = 0;
}

void AXS15231BTouch::setOffsets(uint16_t xRealMin, uint16_t xRealMax, uint16_t xIdealMax,
                                 uint16_t yRealMin, uint16_t yRealMax, uint16_t yIdealMax) {
    _xRealMin = xRealMin;
    _xRealMax = xRealMax;
    _yRealMin = yRealMin;
    _yRealMax = yRealMax;
    _xIdealMax = xIdealMax;
    _yIdealMax = yIdealMax;
}

void AXS15231BTouch::applyOffsetCorrection(uint16_t *x, uint16_t *y) const {
    *x = map(*x, _xRealMin, _xRealMax, 0, _xIdealMax);
    *y = map(*y, _yRealMin, _yRealMax, 0, _yIdealMax);
}

// AXS15231B "read touch point" I2C command frame. Reverse-engineered value
// used by known-working community drivers for this exact controller; the
// vendor does not publish a public register datasheet.
static const uint8_t kReadTouchCmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08};

bool AXS15231BTouch::getPoint(uint16_t *x, uint16_t *y) {
    // NOTE: previously gated on `digitalRead(_intPin) == LOW`, guessing INT
    // stays low for the whole touch. Confirmed wrong on real hardware
    // 2026-09-14: a sustained finger-down produced rapid touch/no-touch
    // flapping at a stable position (every other poll saw INT HIGH), which
    // reset LVGL's press state and broke drags and long-press entirely.
    // INT is evidently a per-report pulse, not a held level — drop the gate
    // and rely on the read itself (rawX==0 && rawY==0 below) for "no touch".
    uint8_t buf[8] = {0};

    Wire.beginTransmission(_addr);
    Wire.write(kReadTouchCmd, sizeof(kReadTouchCmd));
    uint8_t writeErr = Wire.endTransmission();

    int got = (writeErr == 0) ? Wire.requestFrom((int)_addr, (int)sizeof(buf)) : 0;

    // Covers the case where the transaction errors out promptly (NACK, bus
    // busy) rather than blocking outright — recycle the bus before it gets
    // into a worse state. A transaction that genuinely never returns at all
    // is NOT caught by this (control never comes back here to check); that
    // case is the Task Watchdog Timer's job — see setup()/loop() in
    // src/main_ui_demo.cpp. Confirmed necessary on real hardware 2026-09-14:
    // Wire's own setTimeOut() did not prevent a full firmware freeze.
    if (writeErr != 0 || got < (int)sizeof(buf)) {
        if (++_consecutiveErrors >= 5) {
            Serial.printf("[touch] I2C error x%u (writeErr=%u got=%d) -> resetBus()\n", _consecutiveErrors, writeErr,
                          got);
            resetBus();
        }
        return false;
    }
    _consecutiveErrors = 0;

    for (size_t i = 0; i < sizeof(buf) && Wire.available(); i++) {
        buf[i] = Wire.read();
    }

    // buf[2] low nibble + buf[3] = X (12-bit), buf[4] low nibble + buf[5] = Y (12-bit)
    uint16_t rawX = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    uint16_t rawY = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];

    if (rawX == 0 && rawY == 0) {
        _hasLastPoint = false; // genuine finger-up
        _repeatCount = 0;
        // A real (0,0) report means the controller is actively reporting
        // normally again — clear the stuck-latch so a future touch that
        // happens to land on the very same coordinates as the old stuck
        // value still gets a fresh detection window instead of being
        // silently eaten forever (see _confirmedStuck's own comment).
        _confirmedStuck = false;
        return false;
    }

    // Detect the controller wedging: confirmed on real hardware 2026-09-14
    // that after enough rapid polling, it can start returning the exact
    // same nonzero (rawX, rawY) — not just occasional garbage, but forever,
    // every single poll. The "hold last good point" logic right below this
    // exists for a brief bad sample mid-gesture; without this check it
    // instead holds a phantom touch forever once the sensor wedges, which
    // is indistinguishable from the UI simply not responding to anything.
    // ~500ms of an unchanging nonzero reading is well beyond any real touch
    // dwell time at our ~10ms poll rate, so treat it as stuck: force a
    // release, drop the held point, and reset the bus.
    if (rawX == _lastRawX && rawY == _lastRawY) {
        // Already confirmed stuck on this exact value and already tried the
        // one resetBus() attempt for it (below) — discard immediately every
        // poll from here on, WITHOUT re-running the ~500ms counter and
        // WITHOUT calling resetBus() again. Added 2026-09-16 after real
        // hardware logs showed the original code re-triggering "stuck ->
        // resetBus()" every ~500ms forever once truly wedged (see
        // _confirmedStuck's declaration comment) — that resetBus() call
        // never actually helped in that state, it just spammed the log/bus
        // while ALSO still reporting this as a held phantom touch for
        // ~490ms out of every 500ms cycle (the counter had to reach 50
        // again before the old code even noticed). This short-circuits both
        // problems at once.
        if (_confirmedStuck) {
            _hasLastPoint = false;
            return false;
        }
        if (_repeatCount < 0xFFFF) _repeatCount++;
        if (_repeatCount == 50) { // ~500ms at the 10ms indev poll period
            Serial.printf("[touch] stuck: rawX=%u rawY=%u repeated %u times -> resetBus()\n", rawX, rawY,
                          _repeatCount);
            resetBus();
            _hasLastPoint = false;
            _confirmedStuck = true;
            return false;
        }
    } else {
        _lastRawX = rawX;
        _lastRawY = rawY;
        _repeatCount = 0;
        _confirmedStuck = false;
    }

    // REJECT (don't clamp) readings far outside the calibrated range, and
    // hold the last accepted point instead of reporting "no touch" for that
    // one sample. Confirmed on real hardware 2026-09-14: during a fast drag,
    // upward of half the I2C polls returned garbage (rawX/rawY = 4095, or a
    // suspiciously repeated 2859/2859) — a transient bad read, not a real
    // touch near the edge. Clamping turned every one of these into a
    // spurious jump to the screen boundary; outright rejecting (state =
    // RELEASED) turned them into constant drag-interrupting finger-up
    // events instead — both broke drags/sliders ("vào được Settings nhưng
    // không thao tác được gì"). Re-using the last good point keeps the
    // gesture continuous through the bad sample. A real touch is never more
    // than a small overshoot past the calibrated min/max; a 12-bit garbage
    // value is wildly beyond that, so a generous margin still tells them
    // apart safely.
    const uint16_t kMargin = 60;
    bool inRange = !(rawX + kMargin < _xRealMin || rawX > _xRealMax + kMargin || rawY + kMargin < _yRealMin ||
                      rawY > _yRealMax + kMargin);
    if (!inRange) {
        if (!_hasLastPoint) return false;
        *x = _lastX;
        *y = _lastY;
        return true;
    }

    if (rawX > _xRealMax) rawX = _xRealMax;
    if (rawX < _xRealMin) rawX = _xRealMin;
    if (rawY > _yRealMax) rawY = _yRealMax;
    if (rawY < _yRealMin) rawY = _yRealMin;

    uint16_t xMax = _xRealMax, yMax = _yRealMax;
    if (_correctOffset) {
        applyOffsetCorrection(&rawX, &rawY);
        xMax = _xIdealMax;
        yMax = _yIdealMax;
    }

    switch (_rotation) {
        case 0: *x = rawX;        *y = rawY;        break;
        case 1: *x = rawY;        *y = xMax - rawX; break;
        case 2: *x = xMax - rawX; *y = yMax - rawY; break;
        case 3: *x = yMax - rawY; *y = rawX;         break;
        default: *x = rawX;       *y = rawY;         break;
    }

    _lastX = *x;
    _lastY = *y;
    _hasLastPoint = true;
    return true;
}
