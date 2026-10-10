#include "ButtonManager.h"

ButtonManager::ButtonManager() {
    // Left: VOL- (GPIO 39) - instant single click
    m_btnLeft   = { PIN_BTN_VOL_DN, false, 0, false, 0, 0, false, BTN_NONE };
    // Center: BOOT (GPIO 0) - track double click for WiFi toggle
    m_btnCenter = { PIN_BTN_BOOT,   false, 0, false, 0, 0, true,  BTN_NONE };
    // Right: VOL+ (GPIO 40) - instant single click
    m_btnRight  = { PIN_BTN_VOL_UP, false, 0, false, 0, 0, false, BTN_NONE };
}

void ButtonManager::begin() {
    pinMode(PIN_BTN_VOL_DN, INPUT_PULLUP);
    pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
    pinMode(PIN_BTN_VOL_UP, INPUT_PULLUP);
}

void ButtonManager::updateButton(ButtonState &btn) {
    bool rawPressed = (digitalRead(btn.pin) == LOW); // Active LOW
    uint32_t now = millis();

    if (rawPressed) {
        if (!btn.isPressed) {
            // Button just pressed down
            btn.isPressed = true;
            btn.pressStartTime = now;
            btn.longPressFired = false;
        } else {
            // Button is being held down
            if (!btn.longPressFired && (now - btn.pressStartTime >= 650)) {
                btn.longPressFired = true;
                btn.clickCount = 0; // Cancel multi-click if held
                btn.event = BTN_LONG_PRESS;
            }
        }
    } else {
        if (btn.isPressed) {
            // Button just released
            uint32_t pressDuration = now - btn.pressStartTime;
            btn.isPressed = false;

            if (!btn.longPressFired && pressDuration >= 30) {
                if (btn.trackDoubleClick) {
                    btn.clickCount++;
                    btn.lastReleaseTime = now;
                    if (btn.clickCount >= 2) {
                        btn.event = BTN_DOUBLE_CLICK;
                        btn.clickCount = 0;
                    }
                } else {
                    btn.event = BTN_SHORT_CLICK;
                }
            }
        } else {
            // If waiting for second click and window (300ms) expired, fire single click
            if (btn.trackDoubleClick && btn.clickCount == 1 && (now - btn.lastReleaseTime >= 300)) {
                btn.event = BTN_SHORT_CLICK;
                btn.clickCount = 0;
            }
        }
    }
}

void ButtonManager::update() {
    updateButton(m_btnLeft);
    updateButton(m_btnCenter);
    updateButton(m_btnRight);
}

ButtonEvent ButtonManager::getLeftEvent() {
    ButtonEvent ev = m_btnLeft.event;
    m_btnLeft.event = BTN_NONE;
    return ev;
}

ButtonEvent ButtonManager::getCenterEvent() {
    ButtonEvent ev = m_btnCenter.event;
    m_btnCenter.event = BTN_NONE;
    return ev;
}

ButtonEvent ButtonManager::getRightEvent() {
    ButtonEvent ev = m_btnRight.event;
    m_btnRight.event = BTN_NONE;
    return ev;
}
