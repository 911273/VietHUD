#include "SettingsManager.h"
#include "WiFiOTAManager.h"

SettingsManager settings;

SettingsManager::SettingsManager() {
    resetDefaults();
}

void SettingsManager::resetDefaults() {
    data.volume = 85;
    data.brightness = 75;
    data.overspeedAlert = true;
    data.overspeedTolerance = 0;
    data.alertDistance = 350;
    data.signSound = true;
    data.demoMode = false;
    data.defaultSpeedLimit = 60;
    data.hanoiAlertsDb = true;
    data.autoSleepMin = 0;
}

void SettingsManager::begin() {
    load();
}

void SettingsManager::load() {
    if (m_prefs.begin("viethud", true)) {
        data.volume = m_prefs.getUChar("vol", 85);
        data.brightness = m_prefs.getUChar("bri", 75);
        data.overspeedAlert = m_prefs.getBool("spd_alt", true);
        data.overspeedTolerance = m_prefs.getChar("spd_tol", 0);
        data.alertDistance = m_prefs.getUShort("alt_dist", 350);
        data.signSound = m_prefs.getBool("sgn_snd", true);
        data.demoMode = m_prefs.getBool("demo", false);
        data.defaultSpeedLimit = m_prefs.getUChar("def_lim", 60);
        data.hanoiAlertsDb = m_prefs.getBool("hanoi_db", true);
        data.autoSleepMin = m_prefs.getUChar("sleep", 0);
        m_prefs.end();
        Serial.println("[SETTINGS] Da load cau hinh tu bo nho NVS flash.");
    } else {
        Serial.println("[SETTINGS] Chua co cau hinh NVS, su dung mac dinh.");
    }
}

void SettingsManager::save() {
    if (m_prefs.begin("viethud", false)) {
        m_prefs.putUChar("vol", data.volume);
        m_prefs.putUChar("bri", data.brightness);
        m_prefs.putBool("spd_alt", data.overspeedAlert);
        m_prefs.putChar("spd_tol", data.overspeedTolerance);
        m_prefs.putUShort("alt_dist", data.alertDistance);
        m_prefs.putBool("sgn_snd", data.signSound);
        m_prefs.putBool("demo", data.demoMode);
        m_prefs.putUChar("def_lim", data.defaultSpeedLimit);
        m_prefs.putBool("hanoi_db", data.hanoiAlertsDb);
        m_prefs.putUChar("sleep", data.autoSleepMin);
        m_prefs.end();
        Serial.println("[SETTINGS] Da luu cau hinh vao NVS flash.");
    } else {
        Serial.println("[SETTINGS ERROR] Khong the ghi vao NVS flash.");
    }
}

const char* SettingsManager::getItemName(uint8_t index) {
    switch (index) {
        case SETTING_VOLUME:              return "Am luong loa";
        case SETTING_BRIGHTNESS:          return "Do sang man hinh";
        case SETTING_OVERSPEED_ALERT:     return "Bao qua toc do";
        case SETTING_OVERSPEED_TOLERANCE: return "Nguong bu qua toc";
        case SETTING_ALERT_DISTANCE:      return "Cu ly bao truoc";
        case SETTING_SIGN_SOUND:          return "Am bao bien bao";
        case SETTING_STARTUP_MODE:        return "Che do khoi dong";
        case SETTING_DEFAULT_LIMIT:       return "Toc do mac dinh";
        case SETTING_ALERTS_DB:           return "Data bien Ha Noi";
        case SETTING_AUTO_SLEEP:          return "Tu tat man hinh";
        case SETTING_CHECK_OTA_NOW:       return "Cap nhat OTA ngay";
        case SETTING_SYSTEM_INFO:         return "Thong tin he thong";
        case SETTING_RESET_DEFAULTS:      return "Khoi phuc goc";
        default:                          return "Unknown";
    }
}

void SettingsManager::getItemValueStr(uint8_t index, char* buf, size_t bufLen) {
    if (!buf || bufLen == 0) return;

    switch (index) {
        case SETTING_VOLUME:
            snprintf(buf, bufLen, "%d%%", data.volume);
            break;
        case SETTING_BRIGHTNESS:
            snprintf(buf, bufLen, "%d%%", data.brightness);
            break;
        case SETTING_OVERSPEED_ALERT:
            snprintf(buf, bufLen, "%s", data.overspeedAlert ? "BAT" : "TAT");
            break;
        case SETTING_OVERSPEED_TOLERANCE:
            snprintf(buf, bufLen, "+%d km/h", data.overspeedTolerance);
            break;
        case SETTING_ALERT_DISTANCE:
            snprintf(buf, bufLen, "%d m", data.alertDistance);
            break;
        case SETTING_SIGN_SOUND:
            snprintf(buf, bufLen, "%s", data.signSound ? "BAT" : "TAT");
            break;
        case SETTING_STARTUP_MODE:
            snprintf(buf, bufLen, "%s", data.demoMode ? "DEMO" : "LIVE GPS");
            break;
        case SETTING_DEFAULT_LIMIT:
            snprintf(buf, bufLen, "%d km/h", data.defaultSpeedLimit);
            break;
        case SETTING_ALERTS_DB:
            snprintf(buf, bufLen, "%s", data.hanoiAlertsDb ? "40K BIEN" : "TAT");
            break;
        case SETTING_AUTO_SLEEP:
            if (data.autoSleepMin == 0) {
                snprintf(buf, bufLen, "TAT");
            } else {
                snprintf(buf, bufLen, "%d PHUT", data.autoSleepMin);
            }
            break;
        case SETTING_CHECK_OTA_NOW:
            snprintf(buf, bufLen, "[ KICH HOAT ]");
            break;
        case SETTING_SYSTEM_INFO:
            snprintf(buf, bufLen, "v%s", FW_VERSION);
            break;
        case SETTING_RESET_DEFAULTS:
            snprintf(buf, bufLen, "[ RESET ]");
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

bool SettingsManager::isActionItem(uint8_t index) {
    return (index == SETTING_CHECK_OTA_NOW || index == SETTING_RESET_DEFAULTS || index == SETTING_SYSTEM_INFO);
}

void SettingsManager::adjustValue(uint8_t index, bool increment) {
    switch (index) {
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

        case SETTING_STARTUP_MODE:
            data.demoMode = !data.demoMode;
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
