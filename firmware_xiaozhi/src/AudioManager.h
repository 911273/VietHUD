#pragma once

#include <Arduino.h>
#include <driver/i2s.h>
#include "HardwareConfig.h"

enum SoundType : uint8_t {
    SOUND_CLICK = 1,
    SOUND_MODE_SWITCH,
    SOUND_WARN,
    SOUND_OVERSPEED,
    SOUND_STARTUP,
    SOUND_ALERT_CAMERA,
    SOUND_ALERT_TRAFFIC_LIGHT,
    SOUND_ALERT_RESIDENT
};

class AudioManager {
public:
    AudioManager();
    bool begin();
    
    // Non-blocking trigger functions (enqueue sound to Core 0)
    void playClick();
    void playModeSwitch();
    void playWarn();
    void playOverspeed();
    void playStartup();
    void playAlertCamera();
    void playAlertTrafficLight();
    void playAlertResident();

    void setVolume(uint8_t volumePercent); // 0 - 100%

private:
    uint8_t m_volume;
    QueueHandle_t m_queue;

    static void audioTask(void *param);
    void playToneRaw(uint16_t freqHz, uint16_t durationMs);
};

extern AudioManager audio;
