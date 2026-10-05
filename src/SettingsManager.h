#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct SystemSettings {
    uint8_t volume;               // 0 to 100% (default 85)
    uint8_t brightness;           // 25, 50, 75, 100% (default 75)
    bool overspeedAlert;          // true / false (default true)
    int8_t overspeedTolerance;    // 0, 3, 5, 10 km/h (default 0)
    uint16_t alertDistance;       // 200, 300, 350, 400, 500m (default 350)
    bool signSound;               // true / false (default true)
    bool demoMode;                // false = GPS, true = DEMO (default false)
    uint8_t defaultSpeedLimit;    // 40, 50, 60, 70, 80, 90, 100, 120 (default 60)
    bool hanoiAlertsDb;           // true / false (default true)
    uint8_t autoSleepMin;         // 0 (Off), 3, 5, 10 min (default 0)
};

enum SettingItemType : uint8_t {
    SETTING_VOLUME = 0,
    SETTING_BRIGHTNESS,
    SETTING_OVERSPEED_ALERT,
    SETTING_OVERSPEED_TOLERANCE,
    SETTING_ALERT_DISTANCE,
    SETTING_SIGN_SOUND,
    SETTING_STARTUP_MODE,
    SETTING_DEFAULT_LIMIT,
    SETTING_ALERTS_DB,
    SETTING_AUTO_SLEEP,
    SETTING_CHECK_OTA_NOW,
    SETTING_SYSTEM_INFO,
    SETTING_RESET_DEFAULTS,
    SETTING_COUNT
};

class SettingsManager {
public:
    SettingsManager();
    void begin();
    void load();
    void save();
    void resetDefaults();

    SystemSettings data;

    // Helper functions for menu navigation & value adjustment
    const char* getItemName(uint8_t index);
    void getItemValueStr(uint8_t index, char* buf, size_t bufLen);
    void adjustValue(uint8_t index, bool increment);
    bool isActionItem(uint8_t index);

private:
    Preferences m_prefs;
};

extern SettingsManager settings;
