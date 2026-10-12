#pragma once

#include <Arduino.h>
#include "HardwareConfig.h"

// Battery level and charging state, wired as on the Xingzhi Cube 1.54" board
// (same as its xiaozhi-esp32 power_manager.h): battery voltage divider on
// GPIO17 (ADC2_CH6, 12 dB, 12 bit) and charger status on GPIO38 (HIGH = charging).
class BatteryManager {
public:
    void begin();
    void update(uint32_t nowMs);     // samples once per second

    // False when no battery seems connected (USB only): the charger output then
    // floats, so the reading is out of range or jumps around.
    bool isPresent() const { return m_present; }
    bool isCharging() const { return m_charging; }
    uint8_t getLevel() const { return m_level; }   // 0..100 %
    int getRaw() const { return m_avg; }

private:
    static const int kSamples = 10;
    int m_samples[kSamples] = {0};
    int m_count = 0, m_idx = 0;
    int m_avg = 0;
    uint8_t m_level = 0;
    bool m_present = false;
    bool m_charging = false;
    uint32_t m_lastSampleMs = 0;
    uint32_t m_lastLogMs = 0;
};

extern BatteryManager battery;
