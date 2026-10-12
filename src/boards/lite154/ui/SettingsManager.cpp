#include "SettingsManager.h"
#include "WiFiOTAManager.h" // FW_VERSION
#include "core/AppConfig.h"
#include "core/NvsStore.h"
#include "audio/AudioPlayer.h"
#include <Preferences.h>
#include <math.h>

SettingsManager settings;

static uint8_t loadLanguage() {
    Preferences p;
    uint8_t lang = LANG_VI;
    if (p.begin("litui", true)) {
        lang = p.getUChar("lang", LANG_VI);
        p.end();
    }
    return lang;
}

void SettingsManager::begin() { load(); }

void SettingsManager::load() {
    data.volume = (uint8_t)lroundf(cfg.audioVolume);
    data.muted = !cfg.audioEnabled;
    // Lite steps brightness 25/50/75/100; round whatever the portal set.
    int b = (int)lroundf(cfg.brightness);
    data.brightness = b <= 37 ? 25 : b <= 62 ? 50 : b <= 87 ? 75 : 100;
    data.overspeedAlert = cfg.audioOverspeed;
    data.overspeedTolerance = (int8_t)lroundf(cfg.overspeedOffsetKmh);
    data.alertDistance = (uint16_t)lroundf(cfg.warnDistMaxM);
    data.signSound = cfg.audioResident || cfg.audioNoOvertake || cfg.audioToll || cfg.audioLight || cfg.audioDanger;
    data.defaultSpeedLimit = (uint8_t)lroundf(cfg.defaultLimitKmh);
    data.speedCalPct = (int8_t)lroundf(cfg.gnssSpeedCalibrationPct);
    data.language = loadLanguage();
}

void SettingsManager::save() {
    cfg.audioVolume = data.volume;
    cfg.audioEnabled = !data.muted;
    cfg.brightness = data.brightness;
    cfg.audioOverspeed = data.overspeedAlert;
    cfg.overspeedOffsetKmh = data.overspeedTolerance;
    cfg.warnDistMaxM = data.alertDistance;
    cfg.audioResident = cfg.audioNoOvertake = cfg.audioToll = cfg.audioLight = cfg.audioDanger = data.signSound;
    cfg.defaultLimitKmh = data.defaultSpeedLimit;
    cfg.gnssSpeedCalibrationPct = data.speedCalPct;
    clampConfig(cfg);
    saveConfigToNVS(cfg);
    audioSetVolume(data.muted ? 0 : data.volume);
    Preferences p;
    if (p.begin("litui", false)) {
        p.putUChar("lang", data.language);
        p.end();
    }
}

void SettingsManager::resetDefaults() {
    uint8_t lang = data.language;
    AppConfig fresh;
    // Keep the Wi-Fi networks and data source: a "factory reset" of the
    // driving settings must not strand the unit offline.
    memcpy(fresh.savedNetworks, cfg.savedNetworks, sizeof(fresh.savedNetworks));
    fresh.savedNetworkCount = cfg.savedNetworkCount;
    strncpy(fresh.dataUpdateUrl, cfg.dataUpdateUrl, sizeof(fresh.dataUpdateUrl) - 1);
    cfg = fresh;
    load();
    data.language = lang;
}

const char *SettingsManager::getItemName(uint8_t index) {
    switch (index) {
        // Shown in both languages so it can be found whichever one is active
        case SETTING_LANGUAGE:            return "Ngôn ngữ / Language";
        case SETTING_VOLUME:              return L("Âm lượng loa", "Speaker volume");
        case SETTING_BRIGHTNESS:          return L("Độ sáng màn hình", "Screen brightness");
        case SETTING_OVERSPEED_ALERT:     return L("Báo quá tốc độ", "Overspeed alert");
        case SETTING_OVERSPEED_TOLERANCE: return L("Dung sai quá tốc độ", "Overspeed margin");
        case SETTING_SPEED_CAL:           return L("Hiệu chỉnh tốc độ", "Speed calibration");
        case SETTING_ALERT_DISTANCE:      return L("Cự ly báo trước", "Alert distance");
        case SETTING_SIGN_SOUND:          return L("Âm báo biển báo", "Sign sounds");
        case SETTING_DEFAULT_LIMIT:       return L("Tốc độ mặc định", "Default limit");
        case SETTING_CHECK_OTA_NOW:       return L("Cập nhật OTA ngay", "Check OTA now");
        case SETTING_SYSTEM_INFO:         return L("Thông tin hệ thống", "System info");
        case SETTING_RESET_DEFAULTS:      return L("Khôi phục gốc", "Factory reset");
        default:                          return "?";
    }
}

void SettingsManager::getItemValueStr(uint8_t index, char *buf, size_t bufLen) {
    if (!buf || bufLen == 0) return;
    switch (index) {
        case SETTING_LANGUAGE: snprintf(buf, bufLen, "%s", data.language == LANG_EN ? "English" : "Tiếng Việt"); break;
        case SETTING_VOLUME: snprintf(buf, bufLen, "%d%%", data.volume); break;
        case SETTING_BRIGHTNESS: snprintf(buf, bufLen, "%d%%", data.brightness); break;
        case SETTING_OVERSPEED_ALERT: snprintf(buf, bufLen, "%s", data.overspeedAlert ? L("Bật", "On") : L("Tắt", "Off")); break;
        case SETTING_OVERSPEED_TOLERANCE: snprintf(buf, bufLen, "+%d km/h", data.overspeedTolerance); break;
        case SETTING_SPEED_CAL:
            if (liveSpeedKmh >= 0) snprintf(buf, bufLen, "%+d%% = %d km/h", data.speedCalPct, liveSpeedKmh);
            else snprintf(buf, bufLen, "%+d%%", data.speedCalPct);
            break;
        case SETTING_ALERT_DISTANCE: snprintf(buf, bufLen, "%d m", data.alertDistance); break;
        case SETTING_SIGN_SOUND: snprintf(buf, bufLen, "%s", data.signSound ? L("Bật", "On") : L("Tắt", "Off")); break;
        case SETTING_DEFAULT_LIMIT:
            if (data.defaultSpeedLimit == 0) snprintf(buf, bufLen, "%s", L("Tắt", "Off"));
            else snprintf(buf, bufLen, "%d km/h", data.defaultSpeedLimit);
            break;
        case SETTING_CHECK_OTA_NOW: snprintf(buf, bufLen, "%s", L("Kiểm tra", "Check")); break;
        case SETTING_SYSTEM_INFO: snprintf(buf, bufLen, "v%s", FW_VERSION); break;
        case SETTING_RESET_DEFAULTS: snprintf(buf, bufLen, "%s", L("Đặt lại", "Reset")); break;
        default: buf[0] = '\0'; break;
    }
}

bool SettingsManager::isActionItem(uint8_t index) {
    return index == SETTING_CHECK_OTA_NOW || index == SETTING_RESET_DEFAULTS || index == SETTING_SYSTEM_INFO;
}

template <typename T, size_t N> static T stepIn(const T (&vals)[N], T cur, bool inc) {
    int idx = 0;
    for (size_t i = 0; i < N; i++)
        if (vals[i] == cur) idx = (int)i;
    idx = inc ? (idx + 1) % (int)N : (idx + (int)N - 1) % (int)N;
    return vals[idx];
}

void SettingsManager::adjustValue(uint8_t index, bool inc) {
    switch (index) {
        case SETTING_LANGUAGE: data.language = data.language == LANG_EN ? LANG_VI : LANG_EN; break;
        case SETTING_VOLUME: {
            int v = data.volume + (inc ? 15 : -15);
            data.volume = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
            break;
        }
        case SETTING_BRIGHTNESS: {
            static const uint8_t k[] = {25, 50, 75, 100};
            data.brightness = stepIn(k, data.brightness, inc);
            break;
        }
        case SETTING_OVERSPEED_ALERT: data.overspeedAlert = !data.overspeedAlert; break;
        case SETTING_OVERSPEED_TOLERANCE: {
            static const int8_t k[] = {0, 3, 5, 10};
            data.overspeedTolerance = stepIn(k, data.overspeedTolerance, inc);
            break;
        }
        case SETTING_SPEED_CAL: {
            int v = data.speedCalPct + (inc ? 1 : -1);
            data.speedCalPct = (int8_t)(v < -10 ? -10 : v > 10 ? 10 : v);
            break;
        }
        case SETTING_ALERT_DISTANCE: { // max warning distance (speed-based, up to this), <= 400 m
            static const uint16_t k[] = {200, 300, 350, 400};
            data.alertDistance = stepIn(k, data.alertDistance, inc);
            break;
        }
        case SETTING_SIGN_SOUND: data.signSound = !data.signSound; break;
        case SETTING_DEFAULT_LIMIT: {
            static const uint8_t k[] = {0, 40, 50, 60, 70, 80, 90, 100, 120};
            data.defaultSpeedLimit = stepIn(k, data.defaultSpeedLimit, inc);
            break;
        }
        default: break;
    }
}
