#pragma once
#include <Arduino.h>
#include <Preferences.h>

enum UiLanguage : uint8_t { LANG_VI = 0, LANG_EN = 1 };

struct SystemSettings {
    uint8_t volume;               // 0 to 100% (default 85); kept while muted
    bool muted;                   // mute toggle (long press VOL-), survives power off
    uint8_t brightness;           // 25, 50, 75, 100% (default 75)
    bool overspeedAlert;          // true / false (default true)
    int8_t overspeedTolerance;    // 0, 3, 5, 10 km/h (default 0)
    uint16_t alertDistance;       // 200, 300, 350, 400, 500m (default 350)
    bool signSound;               // true / false (default true)
    uint8_t defaultSpeedLimit;    // 40, 50, 60, 70, 80, 90, 100, 120 (default 60)
    bool hanoiAlertsDb;           // true / false (default true)
    uint8_t autoSleepMin;         // 0 (Off), 3, 5, 10 min (default 0)
    uint8_t language;             // UiLanguage (default LANG_VI)
    int8_t speedCalPct;           // -10 .. +10 %: shown speed = GPS speed * (100 + pct) / 100 (default 0)
};

enum SettingItemType : uint8_t {
    SETTING_LANGUAGE = 0,
    SETTING_VOLUME,
    SETTING_BRIGHTNESS,
    SETTING_OVERSPEED_ALERT,
    SETTING_OVERSPEED_TOLERANCE,
    SETTING_SPEED_CAL,
    SETTING_ALERT_DISTANCE,
    SETTING_SIGN_SOUND,
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

    // GPS speed with the user calibration applied.
    int calibratedSpeed(int gpsKmh) const;

    // Calibrated live speed for the settings page preview (-1 = no GPS fix), set by main loop.
    int liveSpeedKmh = -1;

private:
    Preferences m_prefs;
};

extern SettingsManager settings;

// UI string in the current language: L("Tiếng Việt", "English").
inline const char* L(const char* vi, const char* en) {
    return settings.data.language == LANG_EN ? en : vi;
}
