#pragma once

#include <Arduino.h>
#include "HardwareConfig.h"

enum ButtonEvent : uint8_t {
    BTN_NONE = 0,
    BTN_SHORT_CLICK,
    BTN_DOUBLE_CLICK,
    BTN_LONG_PRESS
};

class ButtonManager {
public:
    ButtonManager();
    void begin();
    void update();

    ButtonEvent getLeftEvent();    // VOL- (GPIO 39)
    ButtonEvent getCenterEvent();  // BOOT (GPIO 0)
    ButtonEvent getRightEvent();   // VOL+ (GPIO 40)

private:
    struct ButtonState {
        uint8_t pin;
        bool isPressed;
        uint32_t pressStartTime;
        bool longPressFired;
        uint8_t clickCount;
        uint32_t lastReleaseTime;
        bool trackDoubleClick;
        ButtonEvent event;
    };

    ButtonState m_btnLeft;
    ButtonState m_btnCenter;
    ButtonState m_btnRight;

    void updateButton(ButtonState &btn);
};
