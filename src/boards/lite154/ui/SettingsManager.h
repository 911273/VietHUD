#pragma once
// The Lite's on-device settings menu (same items and button handling as the
// stand-alone Lite firmware) — but every value lives in the platform's own
// AppConfig (core/AppConfig.h, saved by core/NvsStore), so the web portal,
// OTA and the alert engine see the same settings as on every other model.
// Only the UI language is Lite-only (its own NVS key).
#include <Arduino.h>

enum UiLanguage : uint8_t { LANG_VI = 0, LANG_EN = 1 };

struct SystemSettings {
    uint8_t volume;            // 0..100 %, kept while muted   <-> cfg.audioVolume
    bool muted;                //                              <-> !cfg.audioEnabled
    uint8_t brightness;        // 25/50/75/100 %               <-> cfg.brightness
    bool overspeedAlert;       //                              <-> cfg.audioOverspeed
    int8_t overspeedTolerance; // 0/3/5/10 km/h                <-> cfg.overspeedOffsetKmh
    uint16_t alertDistance;    // 50/75/100 m (camera + limit) <-> cfg.cameraWarnDistM/aheadLimitWarnDistM
    bool signSound;            // sign voice/chimes            <-> cfg.audioResident/NoOvertake/Toll/Light/Danger
    uint8_t defaultSpeedLimit; // 0 = off, 40..120             <-> cfg.defaultLimitKmh
    uint8_t language;          // UiLanguage (Lite only)
    int8_t speedCalPct;        // -10..+10 %                   <-> cfg.gnssSpeedCalibrationPct
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
    SETTING_CHECK_OTA_NOW,
    SETTING_SYSTEM_INFO,
    SETTING_RESET_DEFAULTS,
    SETTING_COUNT
};

class SettingsManager {
public:
    void begin();         // after loadConfigFromNVS(): pulls the values from cfg
    void load();          // cfg -> data
    void save();          // data -> cfg, applies, writes NVS
    void resetDefaults(); // platform defaults

    SystemSettings data{};

    const char *getItemName(uint8_t index);
    void getItemValueStr(uint8_t index, char *buf, size_t bufLen);
    void adjustValue(uint8_t index, bool increment);
    bool isActionItem(uint8_t index);

    // The platform applies the calibration in gnss/GNSS.cpp already.
    int calibratedSpeed(int gpsKmh) const { return gpsKmh > 0 ? gpsKmh : 0; }

    int liveSpeedKmh = -1; // settings-page preview, set by LiteUi
};

extern SettingsManager settings;

// UI string in the current language: L("Tiếng Việt", "English").
inline const char *L(const char *vi, const char *en) { return settings.data.language == LANG_EN ? en : vi; }
