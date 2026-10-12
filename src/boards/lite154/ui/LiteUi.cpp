// VietHUD Lite UI on the platform — the stand-alone Lite firmware's loop()
// (apps/lite154/src/main.cpp, 3.5.8) with its data sources swapped for the
// shared core: speed/fix from gnss/GNSS, limit + alerts + road name from
// map/SpeedLimitManager (route-aware matching on the shared speedmap data),
// sounds from core/AlertAudio, settings from core/AppConfig, Wi-Fi from
// net/WebPortal, firmware from net/FwUpdater (release.json, hardware-checked).
// Button mapping is unchanged so existing Lite users don't relearn anything.
#include "LiteUi.h"
#include "BatteryManager.h"
#include "ButtonManager.h"
#include "DisplayManager.h"
#include "SettingsManager.h"
#include "WiFiOTAManager.h"
#include "audio/AudioPlayer.h"
#include "core/AlertAudio.h"
#include "core/AppConfig.h"
#include "core/SharedState.h"
#include "map/SdCardManager.h"
#include "net/DataUpdater.h"
#include "net/FwUpdater.h"
#include "net/WebPortal.h"
#include <WiFi.h>
#include <math.h>

static DisplayManager display;
static ButtonManager buttons;
// `battery` is defined in BatteryManager.cpp

static bool sInSettings = false, sEditing = false, sInSysInfo = false;
static uint8_t sSelIdx = 0;
static int sManualLimit = 0;   // double-click VOL-/VOL+ override while no limit is matched (0 = none)
static uint32_t sLastRender = 0;
static const uint32_t kRenderMs = 50; // 20 FPS, as the stand-alone Lite (heat)

#define ROAD_WAIT_GPS L("Đang chờ GPS…", "Waiting for GPS…")
#define ROAD_UNKNOWN L("Đang dò tên đường…", "Finding road name…")

// ---- Wi-Fi / firmware OTA page (center double-click) ----
static volatile uint8_t sOtaState = OTA_STEP_IDLE;
static volatile int sOtaProgress = 0;
static char sOtaMsg[48] = "";
static FwUpdateInfo sFwInfo;
static uint32_t sOtaDoneMs = 0;

static void otaCheckTask(void *) {
    sOtaState = OTA_STEP_CHECKING_UPDATE;
    bool ok = fwUpdaterCheck(sFwInfo);
    if (ok && sFwInfo.hasUpdate) {
        sOtaState = OTA_STEP_DOWNLOADING;
        snprintf(sOtaMsg, sizeof(sOtaMsg), "v%s", sFwInfo.remoteVersion);
        bool flashed = fwUpdaterStart(sFwInfo.downloadUrl, [](int pct, const char *) {
            if (pct >= 0) sOtaProgress = pct;
            if (pct >= 100) sOtaState = OTA_STEP_REBOOTING;
        }, sFwInfo.sha256); // reboots on success
        if (!flashed) sOtaState = OTA_STEP_FAILED;
    } else if (ok) {
        // Firmware current — fetch newer data the same way the other models do
        // (reboots into the data-update mode, then back).
        dataUpdateCheckStart();
        uint32_t t0 = millis();
        while (dataUpdateCheckInProgress() && millis() - t0 < 20000) vTaskDelay(pdMS_TO_TICKS(200));
        if (dataUpdateAvailable()) {
            snprintf(sOtaMsg, sizeof(sOtaMsg), "DATA %s", dataUpdateRemoteVersion());
            sOtaState = OTA_STEP_DOWNLOADING;
            vTaskDelay(pdMS_TO_TICKS(1500));
            dataUpdateSchedule(); // reboots
        }
        sOtaState = OTA_STEP_UP_TO_DATE;
    } else {
        sOtaState = OTA_STEP_FAILED;
    }
    sOtaDoneMs = millis();
    vTaskDelete(NULL);
}

static void setWifi(bool on) {
    webPortalRequestEnable(on);
    sOtaState = on ? OTA_STEP_CONNECTING_WIFI : OTA_STEP_IDLE;
    sOtaProgress = 0;
    sOtaMsg[0] = '\0';
}

static void otaPoll() {
    if (!webPortalRequestedOn()) {
        sOtaState = OTA_STEP_IDLE;
        return;
    }
    bool sta = WiFi.status() == WL_CONNECTED;
    if (sOtaState == OTA_STEP_CONNECTING_WIFI && sta) {
        sOtaState = OTA_STEP_WIFI_CONNECTED;
        xTaskCreatePinnedToCore(otaCheckTask, "liteOta", 8192, NULL, 1, NULL, 0);
    }
    // Back to the HUD by itself 8 s after a final state, Wi-Fi off.
    if ((sOtaState == OTA_STEP_UP_TO_DATE || sOtaState == OTA_STEP_FAILED) && millis() - sOtaDoneMs > 8000) setWifi(false);
}

// ---- Volume / mute (same behaviour as the stand-alone Lite) ----
static void applyVolume() { audioSetVolume(settings.data.muted ? 0 : settings.data.volume); }

static void showVolumeToast() {
    if (settings.data.muted || settings.data.volume == 0) {
        display.showToast(L("Tắt tiếng", "Muted"), 0xF800);
    } else {
        char t[32];
        snprintf(t, sizeof(t), L("Âm lượng %d%%", "Volume %d%%"), settings.data.volume);
        display.showToast(t, 0x07E0);
    }
}

static void click() {
    if (!settings.data.muted) audioPlayTone(2400, 25);
}

static void toggleMute() {
    settings.data.muted = !settings.data.muted;
    if (!settings.data.muted && settings.data.volume == 0) settings.data.volume = 15;
    settings.save();
    applyVolume();
    click();
    showVolumeToast();
}

void liteUiBegin() {
    settings.begin();
    applyVolume();
    if (!display.begin()) Serial.println("[lite] display init failed");
    display.setBrightness(settings.data.brightness);
    display.setRoadName(ROAD_WAIT_GPS);
    buttons.begin();
    battery.begin();
    Serial.println("[lite] VietHUD Lite UI (no map) running");
}

static void handleButtons() {
    buttons.update();
    ButtonEvent l = buttons.getLeftEvent(), r = buttons.getRightEvent(), c = buttons.getCenterEvent();

    if (sInSettings) {
        if (sInSysInfo) {
            if (c != BTN_NONE || l != BTN_NONE || r != BTN_NONE) { sInSysInfo = false; click(); }
        } else if (c == BTN_LONG_PRESS) { // save & back to the HUD
            sInSettings = sEditing = false;
            settings.save();
            applyVolume();
            display.setBrightness(settings.data.brightness);
            display.showToast(L("Đã lưu cài đặt", "Settings saved"), 0x07E0);
        } else if (c == BTN_SHORT_CLICK) {
            if (sSelIdx == SETTING_CHECK_OTA_NOW) {
                sInSettings = sEditing = false;
                settings.save();
                setWifi(true);
            } else if (sSelIdx == SETTING_RESET_DEFAULTS) {
                settings.resetDefaults();
                settings.save();
                applyVolume();
                display.setBrightness(settings.data.brightness);
                display.showToast(L("Đã khôi phục gốc", "Defaults restored"), 0x07E0);
            } else if (sSelIdx == SETTING_SYSTEM_INFO) {
                sInSysInfo = true;
            } else {
                sEditing = !sEditing;
            }
            click();
        } else if (l == BTN_SHORT_CLICK || r == BTN_SHORT_CLICK) {
            bool inc = r == BTN_SHORT_CLICK;
            if (sEditing) {
                settings.adjustValue(sSelIdx, inc);
                if (sSelIdx == SETTING_VOLUME) applyVolume();
                else if (sSelIdx == SETTING_BRIGHTNESS) display.setBrightness(settings.data.brightness);
            } else {
                sSelIdx = inc ? (sSelIdx + 1) % SETTING_COUNT : (sSelIdx == 0 ? SETTING_COUNT - 1 : sSelIdx - 1);
            }
            click();
        }
        return;
    }

    // HUD mode
    if (c == BTN_LONG_PRESS) {
        settings.load(); // pick up anything changed from the web portal
        sInSettings = true;
        sEditing = false;
        click();
    } else if (c == BTN_DOUBLE_CLICK) {
        setWifi(!webPortalRequestedOn());
        click();
    } else if (c == BTN_SHORT_CLICK) {
        if (webPortalRequestedOn()) setWifi(false);
        else toggleMute();
    }

    if (l == BTN_SHORT_CLICK) {
        if (!settings.data.muted) {
            settings.data.volume = settings.data.volume > 15 ? settings.data.volume - 15 : 0;
            settings.save();
            applyVolume();
            click();
        }
        showVolumeToast();
    } else if (l == BTN_LONG_PRESS) {
        toggleMute();
    } else if (l == BTN_DOUBLE_CLICK || r == BTN_DOUBLE_CLICK) {
        int base = sManualLimit > 0 ? sManualLimit : (cfg.defaultLimitKmh > 0 ? (int)cfg.defaultLimitKmh : 60);
        sManualLimit = constrain(base + (r == BTN_DOUBLE_CLICK ? 10 : -10), 30, 120);
        click();
        char t[32];
        snprintf(t, sizeof(t), "%d km/h", sManualLimit);
        display.showToast(t, 0x07FF);
    }

    if (r == BTN_SHORT_CLICK) {
        if (settings.data.muted && settings.data.volume > 0) settings.data.muted = false; // first press just unmutes
        else {
            settings.data.muted = false;
            settings.data.volume = settings.data.volume + 15 > 100 ? 100 : settings.data.volume + 15;
        }
        settings.save();
        applyVolume();
        click();
        showVolumeToast();
    } else if (r == BTN_LONG_PRESS) {
        display.cycleBrightness();
        settings.data.brightness = (display.getBrightness() * 100 + 127) / 255;
        settings.save();
        click();
    }
}

// The next thing on the road for the alert card: nearest of camera / sign /
// tunnel ahead (all along-route distances from map/RoutePredictor).
static bool pickAlert(const RoadInfoSnapshot &road, uint8_t *type, uint16_t *dist, int *limit) {
    float best = 1e9f;
    *type = 0;
    auto take = [&](bool valid, float d, uint8_t t) {
        if (valid && d >= 0 && d < best) { best = d; *type = t; }
    };
    take(road.cameraAheadValid, road.cameraAheadDistanceM, 1);
    take(road.residentAreaAheadValid, road.residentAreaAheadDistM, 2);
    take(road.noOvertakingAheadValid, road.noOvertakingAheadDistM, 3);
    take(road.tollBoothAheadValid, road.tollBoothAheadDistM, 5);
    take(road.trafficLightAheadValid, road.trafficLightAheadDistM, 6);
    take(road.dangerAheadValid, road.dangerAheadDistM, 10);
    take(road.tunnelAheadValid, road.tunnelAheadDistM, 10);
    if (!*type) return false;
    *dist = (uint16_t)lroundf(best);
    if (*type == 1 && road.cameraSpeedLimitKmh > 0) *limit = (int)lroundf(road.cameraSpeedLimitKmh);
    return true;
}

void liteUiTick() {
    uint32_t now = millis();
    handleButtons();
    otaPoll();
    battery.update(now);
    display.setBattery(battery.isPresent(), battery.getLevel(), battery.isCharging());

    GnssSnapshot gnss = gnssSnapshot();
    RoadInfoSnapshot road = roadInfoSnapshot();
    bool speeding = alertIsSpeeding(gnss, road);
    if (!sInSettings && sOtaState == OTA_STEP_IDLE) alertAudioTick(gnss, road, speeding);
    if (road.valid) sManualLimit = 0; // a matched limit wins over a manual one

    int speed = gnss.speedValid ? (int)lroundf(gnss.egoSpeedKmh) : 0;
    settings.liveSpeedKmh = gnss.fix ? speed : -1;
    int limit = road.valid && road.speedLimitKmh > 0 ? (int)lroundf(road.speedLimitKmh)
                : sManualLimit > 0                    ? sManualLimit
                : gnss.fix && cfg.defaultLimitKmh > 0 ? (int)cfg.defaultLimitKmh
                                                      : 0;
    uint8_t aType = 0;
    uint16_t aDist = 0;
    bool aActive = gnss.fix && pickAlert(road, &aType, &aDist, &limit);
    if (!gnss.fix) display.setRoadName(ROAD_WAIT_GPS);
    else display.setRoadName(road.roadName[0] ? road.roadName : ROAD_UNKNOWN);

    if (now - sLastRender < kRenderMs) return;
    sLastRender = now;

    if (sInSettings && sInSysInfo) {
        SystemInfoData info = {
            "UBX", (uint32_t)GNSS_BAUD, gnss.satCount, gnss.fix, gnss.hdop,
            (uint32_t)0, (uint32_t)0, battery.isPresent(), battery.getLevel(), battery.isCharging(), (uint32_t)0,
        };
        display.renderSystemInfo(info);
    } else if (sInSettings) {
        display.renderSettings(sSelIdx, sEditing);
    } else {
        bool wifiOn = webPortalRequestedOn();
        bool sta = WiFi.status() == WL_CONNECTED;
        char ip[24] = "";
        if (sta) snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
        display.updateWiFiState(wifiOn, sOtaState, sOtaMsg, sta ? WiFi.SSID().c_str() : "", ip,
                                sta ? WiFi.RSSI() : 0, sOtaProgress, 0);
        display.updateData(speed, limit, gnss.satCount, gnss.fix, settings.data.muted, nullptr, wifiOn, sta, ip,
                           sOtaState == OTA_STEP_DOWNLOADING, sOtaProgress, aActive, aType, aDist);
        display.render();
    }
}

// ---- Status page (data-update mode / install result) ----
static char sStTitle[40] = "", sStMsg[96] = "";
static int sStPct = -1;
static int sStResult = 0; // 0 running, 1 ok, -1 failed
static uint32_t sStLastDraw = 0;

void liteStatusOpen(const char *title, uint32_t) {
    snprintf(sStTitle, sizeof(sStTitle), "%s", title);
    sStMsg[0] = '\0';
    sStPct = -1;
    sStResult = 0;
    sStLastDraw = 0;
}
void liteStatusIcon(bool ok, uint32_t) { sStResult = ok ? 1 : -1; }
void liteStatusMsg(const char *text) { snprintf(sStMsg, sizeof(sStMsg), "%s", text ? text : ""); }
void liteStatusBar(int pct) { sStPct = pct; }
void liteStatusSub(const char *) {}
void liteStatusHint(const char *) {}
void liteStatusPump() {
    if (millis() - sStLastDraw < 200) return;
    sStLastDraw = millis();
    if (sStResult > 0) display.showOtaSuccess(sStMsg, 0);
    else if (sStResult < 0) display.showOtaFailure(sStMsg);
    else display.showOtaProgress(sStTitle, sStPct < 0 ? 0 : sStPct, sStMsg);
}
