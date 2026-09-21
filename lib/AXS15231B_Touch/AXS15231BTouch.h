#pragma once
#include <Arduino.h>
#include <Wire.h>

// Minimal I2C touch driver for the AXS15231B combo LCD/touch controller
// used on the JC3248W535 board.
//
// Polling design: getPoint() issues a fresh I2C read every call and treats
// (x=0, y=0) as "no touch". An earlier version gated the read on
// `digitalRead(intPin) == LOW`, guessing INT stays held low for the whole
// touch — confirmed wrong on real hardware 2026-09-14 (INT is evidently a
// per-report pulse, not a held level: the gate produced rapid touch/no-touch
// flapping under a sustained finger, breaking drags and long-press). The
// _intPin is currently unused as a result; revisit with an oscilloscope/
// logic analyzer if interrupt-driven (rather than polled) touch is wanted
// later.
//
// Bus self-recovery: `Wire.setTimeOut()` was tried as a guard against a
// stuck bus and did NOT prevent a full firmware freeze on real hardware
// (confirmed 2026-09-14, reproduced with a held drag in Settings) — its
// documented 50ms default was already in effect and made no difference,
// meaning the legacy ESP32 Arduino core 2.x I2C driver doesn't reliably
// honor it for this stall pattern. getPoint() instead checks
// endTransmission()/requestFrom() return codes itself and reinitializes the
// whole Wire bus after a few consecutive failures, since a wedged I2C bus
// is a real, somewhat expected failure mode under sustained rapid polling
// and needs an explicit recovery path rather than trusting the library.
// See docs/V1.2_hardening_proposal.md for the full investigation.
class AXS15231BTouch {
public:
    AXS15231BTouch(uint8_t sda, uint8_t scl, uint8_t intPin, uint8_t addr)
        : _sda(sda), _scl(scl), _intPin(intPin), _addr(addr) {}

    bool begin();

    // Returns true and fills x/y (already rotated + offset-corrected) if the
    // panel is currently touched.
    bool getPoint(uint16_t *x, uint16_t *y);

    void setRotation(uint8_t rotation) { _rotation = rotation; }
    void enableOffsetCorrection(bool en) { _correctOffset = en; }
    void setOffsets(uint16_t xRealMin, uint16_t xRealMax, uint16_t xIdealMax,
                     uint16_t yRealMin, uint16_t yRealMax, uint16_t yIdealMax);

private:
    void applyOffsetCorrection(uint16_t *x, uint16_t *y) const;
    void resetBus();

    uint8_t _sda, _scl, _intPin, _addr;
    uint8_t _rotation = 0;
    uint8_t _consecutiveErrors = 0;

    bool _correctOffset = false;
    uint16_t _xRealMin = 0, _xRealMax = 0, _yRealMin = 0, _yRealMax = 0;
    uint16_t _xIdealMax = 0, _yIdealMax = 0;

    // Last accepted (in-range) point, held over the next sample or two if a
    // garbage read comes in mid-touch — see getPoint().
    uint16_t _lastX = 0, _lastY = 0;
    bool _hasLastPoint = false;

    // Detects the controller wedging into repeating the exact same raw
    // reading forever (confirmed on real hardware 2026-09-14 — see
    // getPoint()), as opposed to genuinely brief garbage blips.
    uint16_t _lastRawX = 0, _lastRawY = 0;
    uint16_t _repeatCount = 0;

    // Latches once getPoint() has already tried resetBus() for the CURRENT
    // repeating raw value (added 2026-09-16 — see docs/V1.2_hardening_proposal.md
    // section 5c: a controller wedged at its own internal level returns the
    // identical value again immediately after a bus reset, since resetBus()
    // only recycles the ESP32-side I2C peripheral, not the chip itself).
    // Without this, getPoint() re-ran the full ~500ms detection window and
    // called resetBus() again every single cycle forever — pure log/bus
    // churn with no chance of actually helping the second time. Cleared the
    // moment the raw reading changes (a real touch, or the chip recovering
    // on its own), so detection is live again for any future episode.
    bool _confirmedStuck = false;
};
