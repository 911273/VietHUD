#include "SettingsManager.h"
#include "WiFiOTAManager.h"

SettingsManager settings;

SettingsManager::SettingsManager() {
    resetDefaults();
}

void SettingsManager::resetDefaults() {
    data.volume = 85;
    data.muted = false;
    data.brightness = 75;
    data.overspeedAlert = true;
    data.overspeedTolerance = 0;
    data.alertDistance = 350;
    data.signSound = true;
    data.defaultSpeedLimit = 60;
    data.hanoiAlertsDb = true;
    data.autoSleepMin = 0;
    data.speedCalPct = 0;
    data.language = LANG_VI;
}

void SettingsManager::begin() {
    load();
}

void SettingsManager::load() {
    if (m_prefs.begin("viethud", true)) {
        data.volume = m_prefs.getUChar("vol", 85);
        data.muted = m_prefs.getBool("muted", false);
        data.brightness = m_prefs.getUChar("bri", 75);
        data.overspeedAlert = m_prefs.getBool("spd_alt", true);
        data.overspeedTolerance = m_prefs.getChar("spd_tol", 0);
        data.alertDistance = m_prefs.getUShort("alt_dist", 350);
        data.signSound = m_prefs.getBool("sgn_snd", true);
        data.defaultSpeedLimit = m_prefs.getUChar("def_lim", 60);
        data.hanoiAlertsDb = m_prefs.getBool("hanoi_db", true);
        data.autoSleepMin = m_prefs.getUChar("sleep", 0);
        data.speedCalPct = m_prefs.getChar("spd_cal", 0);
        data.language = m_prefs.getUChar("lang", LANG_VI);
        m_prefs.end();
        Serial.println("[SETTINGS] Da load cau hinh tu bo nho NVS flash.");
    } else {
        Serial.println("[SETTINGS] Chua co cau hinh NVS, su dung mac dinh.");
    }
}

void SettingsManager::save() {
    if (m_prefs.begin("viethud", false)) {
        m_prefs.putUChar("vol", data.volume);
        m_prefs.putBool("muted", data.muted);
        m_prefs.putUChar("bri", data.brightness);
        m_prefs.putBool("spd_alt", data.overspeedAlert);
        m_prefs.putChar("spd_tol", data.overspeedTolerance);
        m_prefs.putUShort("alt_dist", data.alertDistance);
        m_prefs.putBool("sgn_snd", data.signSound);
        m_prefs.putUChar("def_lim", data.defaultSpeedLimit);
        m_prefs.putBool("hanoi_db", data.hanoiAlertsDb);
        m_prefs.putUChar("sleep", data.autoSleepMin);
        m_prefs.putChar("spd_cal", data.speedCalPct);
        m_prefs.putUChar("lang", data.language);
        m_prefs.end();
        Serial.println("[SETTINGS] Da luu cau hinh vao NVS flash.");
    } else {
        Serial.println("[SETTINGS ERROR] Khong the ghi vao NVS flash.");
    }
}

const char* SettingsManager::getItemName(uint8_t index) {
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
        case SETTING_ALERTS_DB:           return L("Dữ liệu cảnh báo", "Alert database");
        case SETTING_AUTO_SLEEP:          return L("Tự tắt màn hình", "Screen auto-off");
        case SETTING_CHECK_OTA_NOW:       return L("Cập nhật OTA ngay", "Check OTA now");
        case SETTING_SYSTEM_INFO:         return L("Thông tin hệ thống", "System info");
        case SETTING_RESET_DEFAULTS:      return L("Khôi phục gốc", "Factory reset");
        default:                          return "?";
    }
}

void SettingsManager::getItemValueStr(uint8_t index, char* buf, size_t bufLen) {
    if (!buf || bufLen == 0) return;

    switch (index) {
        case SETTING_LANGUAGE:
            snprintf(buf, bufLen, "%s", data.language == LANG_EN ? "English" : "Tiếng Việt");
            break;
        case SETTING_VOLUME:
            snprintf(buf, bufLen, "%d%%", data.volume);
            break;
        case SETTING_BRIGHTNESS:
            snprintf(buf, bufLen, "%d%%", data.brightness);
            break;
        case SETTING_OVERSPEED_ALERT:
            snprintf(buf, bufLen, "%s", data.overspeedAlert ? L("Bật", "On") : L("Tắt", "Off"));
            break;
        case SETTING_OVERSPEED_TOLERANCE:
            snprintf(buf, bufLen, "+%d km/h", data.overspeedTolerance);
            break;
        case SETTING_SPEED_CAL:
            if (liveSpeedKmh >= 0) {
                snprintf(buf, bufLen, "%+d%% = %d km/h", data.speedCalPct, liveSpeedKmh);
            } else {
                snprintf(buf, bufLen, "%+d%%", data.speedCalPct);
            }
            break;
        case SETTING_ALERT_DISTANCE:
            snprintf(buf, bufLen, "%d m", data.alertDistance);
            break;
        case SETTING_SIGN_SOUND:
            snprintf(buf, bufLen, "%s", data.signSound ? L("Bật", "On") : L("Tắt", "Off"));
            break;
        case SETTING_DEFAULT_LIMIT:
            snprintf(buf, bufLen, "%d km/h", data.defaultSpeedLimit);
            break;
        case SETTING_ALERTS_DB:
            snprintf(buf, bufLen, "%s", data.hanoiAlertsDb ? L("Bật", "On") : L("Tắt", "Off"));
            break;
        case SETTING_AUTO_SLEEP:
            if (data.autoSleepMin == 0) {
                snprintf(buf, bufLen, "%s", L("Tắt", "Off"));
            } else {
                snprintf(buf, bufLen, L("%d phút", "%d min"), data.autoSleepMin);
            }
            break;
        case SETTING_CHECK_OTA_NOW:
            snprintf(buf, bufLen, "%s", L("Kiểm tra", "Check"));
            break;
        case SETTING_SYSTEM_INFO:
            snprintf(buf, bufLen, "v%s", FW_VERSION);
            break;
        case SETTING_RESET_DEFAULTS:
            snprintf(buf, bufLen, "%s", L("Đặt lại", "Reset"));
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

int SettingsManager::calibratedSpeed(int gpsKmh) const {
    if (gpsKmh <= 0) return 0;
    return (gpsKmh * (100 + data.speedCalPct) + 50) / 100;
}

bool SettingsManager::isActionItem(uint8_t index) {
    return (index == SETTING_CHECK_OTA_NOW || index == SETTING_RESET_DEFAULTS || index == SETTING_SYSTEM_INFO);
}

void SettingsManager::adjustValue(uint8_t index, bool increment) {
    switch (index) {
        case SETTING_LANGUAGE:
            data.language = (data.language == LANG_EN) ? LANG_VI : LANG_EN;
            break;

        case SETTING_VOLUME: {
            int v = data.volume;
            v += (increment ? 15 : -15);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            data.volume = (uint8_t)v;
            break;
        }

        case SETTING_BRIGHTNESS: {
            const uint8_t levels[] = {25, 50, 75, 100};
            int curIdx = 2; // 75%
            for (int i = 0; i < 4; i++) {
                if (data.brightness == levels[i]) curIdx = i;
            }
            if (increment) {
                curIdx = (curIdx + 1) % 4;
            } else {
                curIdx = (curIdx + 3) % 4;
            }
            data.brightness = levels[curIdx];
            break;
        }

        case SETTING_OVERSPEED_ALERT:
            data.overspeedAlert = !data.overspeedAlert;
            break;

        case SETTING_OVERSPEED_TOLERANCE: {
            const int8_t tols[] = {0, 3, 5, 10};
            int curIdx = 0;
            for (int i = 0; i < 4; i++) {
                if (data.overspeedTolerance == tols[i]) curIdx = i;
            }
            if (increment) {
                curIdx = (curIdx + 1) % 4;
            } else {
                curIdx = (curIdx + 3) % 4;
            }
            data.overspeedTolerance = tols[curIdx];
            break;
        }

        case SETTING_SPEED_CAL: {
            int v = data.speedCalPct + (increment ? 1 : -1);
            if (v < -10) v = -10;
            if (v > 10) v = 10;
            data.speedCalPct = (int8_t)v;
            break;
        }

        case SETTING_ALERT_DISTANCE: {
            const uint16_t dists[] = {200, 300, 350, 400, 500};
            int curIdx = 2; // 350m
            for (int i = 0; i < 5; i++) {
                if (data.alertDistance == dists[i]) curIdx = i;
            }
            if (increment) {
                curIdx = (curIdx + 1) % 5;
            } else {
                curIdx = (curIdx + 4) % 5;
            }
            data.alertDistance = dists[curIdx];
            break;
        }

        case SETTING_SIGN_SOUND:
            data.signSound = !data.signSound;
            break;

        case SETTING_DEFAULT_LIMIT: {
            const uint8_t limits[] = {40, 50, 60, 70, 80, 90, 100, 120};
            int curIdx = 2; // 60
            for (int i = 0; i < 8; i++) {
                if (data.defaultSpeedLimit == limits[i]) curIdx = i;
            }
            if (increment) {
                curIdx = (curIdx + 1) % 8;
            } else {
                curIdx = (curIdx + 7) % 8;
            }
            data.defaultSpeedLimit = limits[curIdx];
            break;
        }

        case SETTING_ALERTS_DB:
            data.hanoiAlertsDb = !data.hanoiAlertsDb;
            break;

        case SETTING_AUTO_SLEEP: {
            const uint8_t sleeps[] = {0, 3, 5, 10};
            int curIdx = 0;
            for (int i = 0; i < 4; i++) {
                if (data.autoSleepMin == sleeps[i]) curIdx = i;
            }
            if (increment) {
                curIdx = (curIdx + 1) % 4;
            } else {
                curIdx = (curIdx + 3) % 4;
            }
            data.autoSleepMin = sleeps[curIdx];
            break;
        }

        default:
            break;
    }
}
