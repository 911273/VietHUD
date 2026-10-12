#include "BatteryManager.h"

BatteryManager battery;

namespace {
// Raw ADC -> % table from the board's original firmware (xiaozhi power_manager.h)
const struct { int adc; uint8_t level; } kLevels[] = {
    {1970, 0}, {2062, 20}, {2154, 40}, {2246, 60}, {2338, 80}, {2430, 100},
};
const int kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);
const int kMinPlausibleRaw = 1700;   // below ~empty cell: nothing on the battery pins
const int kMaxPlausibleRaw = 2700;
const int kMaxSpreadRaw = 250;       // floating charger output jumps far more than a real cell

uint8_t rawToLevel(int raw) {
    if (raw <= kLevels[0].adc) return 0;
    if (raw >= kLevels[kLevelCount - 1].adc) return 100;
    for (int i = 0; i < kLevelCount - 1; i++) {
        if (raw < kLevels[i + 1].adc) {
            float r = (float)(raw - kLevels[i].adc) / (kLevels[i + 1].adc - kLevels[i].adc);
            return (uint8_t)(kLevels[i].level + r * (kLevels[i + 1].level - kLevels[i].level) + 0.5f);
        }
    }
    return 100;
}
} // namespace

void BatteryManager::begin() {
    pinMode(PIN_BAT_CHARGING, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);   // = 12 dB on ESP32-S3
    update(0);
}

void BatteryManager::update(uint32_t nowMs) {
    if (m_count > 0 && nowMs - m_lastSampleMs < 1000) return;
    m_lastSampleMs = nowMs;

    m_samples[m_idx] = analogRead(PIN_BAT_ADC);
    m_idx = (m_idx + 1) % kSamples;
    if (m_count < kSamples) m_count++;

    // Level from the last 5 samples; presence from the spread over all 10
    int sum = 0, n = m_count < 5 ? m_count : 5;
    int lo = 4095, hi = 0;
    for (int i = 0; i < m_count; i++) {
        int v = m_samples[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    for (int i = 0; i < n; i++) sum += m_samples[(m_idx - 1 - i + kSamples) % kSamples];
    m_avg = sum / n;

    m_charging = digitalRead(PIN_BAT_CHARGING) == HIGH;
    m_present = m_avg >= kMinPlausibleRaw && m_avg <= kMaxPlausibleRaw && (hi - lo) <= kMaxSpreadRaw;
    m_level = rawToLevel(m_avg);

    if (nowMs - m_lastLogMs >= 10000 || m_lastLogMs == 0) {
        m_lastLogMs = nowMs ? nowMs : 1;
        Serial.printf("[BAT] raw=%d (min %d max %d) muc=%d%% sac=%d co_pin=%d\n",
                      m_avg, lo, hi, m_level, m_charging, m_present);
    }
}
