#include <Arduino.h>
#include "HardwareConfig.h"
#include "DisplayManager.h"
#include "GPSManager.h"
#include "ButtonManager.h"
#include "AudioManager.h"
#include "WiFiOTAManager.h"
#include "TrafficAlertManager.h"
#include "SettingsManager.h"
#include "RoadNameManager.h"
#include "BatteryManager.h"
#include <esp_system.h>

DisplayManager display;
GPSManager gps;
ButtonManager buttons;

// State variables
int currentSpeed = 0;
int speedLimit = 60;
bool isMuted = false;   // mirrors settings.data.muted (or a saved volume of 0)

// Settings state
bool inSettingsPage = false;
bool isEditingSetting = false;
bool inSystemInfo = false;     // Settings > System info page open
uint8_t settingSelectedIndex = 0;

uint32_t lastRenderTime = 0;
// Thermal: the loop used to spin flat out and redraw at 40 FPS (chip ~57 C on the desk).
// 20 FPS is plenty for this UI, and a short idle each loop lets the core sleep.
uint16_t renderIntervalMs = 50;
const uint8_t kLoopIdleMs = 2;
uint32_t lastTempLogTime = 0;
uint32_t lastOverspeedAlertTime = 0;
uint32_t lastSoundAlertTime = 0;
int32_t lastAnnouncedAlertIdx = -1;

#define ROAD_WAIT_GPS L("Đang chờ GPS…", "Waiting for GPS…")
#define ROAD_UNKNOWN  L("Đang dò tên đường…", "Finding road name…")

// Manual alert override via Serial for testing
bool manualAlertActive = false;
uint8_t manualAlertType = 0;
uint16_t manualAlertDist = 0;

// Speaker follows the saved level unless muted. settings.data.volume keeps the level
// while muted, so unmuting (or a power cycle) comes back to exactly where it was.
void applyVolume() {
    isMuted = settings.data.muted || settings.data.volume == 0;
    audio.setVolume(isMuted ? 0 : settings.data.volume);
}

void showVolumeToast() {
    if (isMuted) {
        display.showToast(L("Tắt tiếng", "Muted"), 0xF800);
    } else {
        char toast[32];
        snprintf(toast, sizeof(toast), L("Âm lượng %d%%", "Volume %d%%"), settings.data.volume);
        display.showToast(toast, 0x07E0);
    }
}

// Quick mute toggle (center short press, or hold VOL-). Saved, so it survives power off.
void toggleMute() {
    settings.data.muted = !isMuted;
    if (!settings.data.muted && settings.data.volume == 0) settings.data.volume = 15;
    settings.save();
    applyVolume();
    if (!isMuted) audio.playClick();
    showVolumeToast();
    Serial.printf("[BUTTON] Mute: %s\n", isMuted ? "MUTED" : "UNMUTED");
}

void handleSerialCommands() {
    while (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        if (line.startsWith("ROAD=")) {
            String road = line.substring(5);
            display.setRoadName(road.c_str());
            Serial.printf("[CMD] Cap nhat ten duong: %s\n", road.c_str());
        } else if (line.startsWith("LIMIT=")) {
            speedLimit = line.substring(6).toInt();
            char toast[32];
            snprintf(toast, sizeof(toast), "%d km/h", speedLimit);
            display.showToast(toast, 0x07FF);
            Serial.printf("[CMD] Cap nhat gioi han: %d km/h\n", speedLimit);
        } else if (line.startsWith("SPEED=")) {
            currentSpeed = line.substring(6).toInt();
            Serial.printf("[CMD] Cap nhat toc do: %d km/h\n", currentSpeed);
        } else if (line.startsWith("ALERT=")) {
            String param = line.substring(6);
            if (param.equalsIgnoreCase("OFF")) {
                manualAlertActive = false;
                Serial.println("[CMD] Tat canh bao manual.");
            } else {
                int comma = param.indexOf(',');
                String icon = (comma > 0) ? param.substring(0, comma) : param;
                int dist = (comma > 0) ? param.substring(comma + 1).toInt() : 350;

                manualAlertActive = true;
                manualAlertDist = dist;

                if (icon.equalsIgnoreCase("CAM") || icon == "1") {
                    manualAlertType = ALERT_SPEED_CAMERA;
                    audio.playAlertCamera();
                } else if (icon.equalsIgnoreCase("RED") || icon == "6") {
                    manualAlertType = ALERT_TRAFFIC_LIGHT;
                    audio.playAlertTrafficLight();
                } else if (icon.equalsIgnoreCase("RES") || icon == "2") {
                    manualAlertType = ALERT_RESIDENT_AREA;
                    audio.playAlertResident();
                } else if (icon.equalsIgnoreCase("NOV") || icon == "3") {
                    manualAlertType = ALERT_NO_OVERTAKING;
                    audio.playWarn();
                } else if (icon.equalsIgnoreCase("BOT") || icon == "5") {
                    manualAlertType = ALERT_TOLL_BOOTH;
                    audio.playWarn();
                } else {
                    manualAlertType = ALERT_DANGER;
                    audio.playWarn();
                }
                Serial.printf("[CMD] Kich hoat alert: Type %d (%dm)\n", manualAlertType, manualAlertDist);
            }
        } else if (line.equalsIgnoreCase("WIFI")) {
            bool wifiState = wifiOta.toggleWiFi();
            Serial.printf("[CMD] WiFi: %s\n", wifiState ? "ON (CHUYEN TRANG KET NOI & OTA)" : "OFF (QUAY VE HUD)");
        } else if (line.startsWith("VEL=")) {
            gps.rawVelLog = line.substring(4).toInt() != 0;
        } else if (line.startsWith("CPU=")) {
            int mhz = line.substring(4).toInt();
            if (mhz == 80 || mhz == 160 || mhz == 240) setCpuFrequencyMhz(mhz);
            Serial.printf("[CMD] CPU: %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
        } else if (line.startsWith("FPS=")) {
            int fps = line.substring(4).toInt();
            if (fps >= 5 && fps <= 50) renderIntervalMs = 1000 / fps;
            Serial.printf("[CMD] Render: %d ms/khung\n", renderIntervalMs);
        } else if (line.equalsIgnoreCase("OTA")) {
            Serial.println("[CMD] Kiem tra cap nhat OTA tu Pi 4...");
            display.showToast(L("Kiểm tra OTA…", "Checking OTA…"), 0x07FF);
            wifiOta.checkForUpdate();
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("=========================================");
    Serial.println(" VIETHUD LITE v3.5.8 - GIAM NHIET         ");
    Serial.println("=========================================");
    Serial.printf("[BOOT] Ly do khoi dong: %d\n", (int)esp_reset_reason());
    setCpuFrequencyMhz(160);   // plenty for 20 FPS; less heat than 240 MHz

    // 0. Initialize Settings from NVS flash
    settings.begin();
    applyVolume();
    Serial.printf("[AUDIO] Am luong da luu: %d%%%s\n", settings.data.volume, isMuted ? " (TAT TIENG)" : "");
    display.setBrightness(settings.data.brightness);
    speedLimit = settings.data.defaultSpeedLimit;

    // 1. Initialize display & hardware
    if (!display.begin()) {
        Serial.println("[ERROR] Display initialization failed!");
    } else {
        Serial.println("[OK] Display initialized successfully.");
        display.setBrightness(settings.data.brightness);
    }

    display.setRoadName(ROAD_WAIT_GPS);

    // 2. Initialize Audio I2S
    if (audio.begin()) {
        Serial.println("[OK] Audio I2S initialized on DOUT=7, BCLK=15, LRCK=16.");
        applyVolume();
        audio.playStartup();
    } else {
        Serial.println("[ERROR] Audio I2S initialization failed!");
    }

    // 3. Initialize GPS
    gps.begin(GPS_BAUD_DEFAULT);
    Serial.println("[OK] GPS UART initialized on RX=44, TX=43.");

    // 4. Initialize Buttons
    buttons.begin();
    Serial.println("[OK] Buttons initialized (BOOT:0, VOL+:40, VOL-:39).");

    // 5. Initialize WiFi & OTA Manager
    wifiOta.begin();
    Serial.println("[OK] WiFi & OTA initialized (Default APs: iPhone cua Pham, VuPQ).");

    // 6. Initialize Hanoi 200km Traffic Alerts database
    trafficAlerts.begin();

    // 7. Road names (embedded OSM database)
    roadNames.begin();

    // 8. Battery level / charging
    battery.begin();

    lastRenderTime = millis();
}

void loop() {
    uint32_t now = millis();

    // 1. Check Serial command inputs
    handleSerialCommands();

    // 2. Update WiFi & OTA handles
    wifiOta.update();

    // 3. Process GPS stream
    gps.update();

    battery.update(now);
    display.setBattery(battery.isPresent(), battery.getLevel(), battery.isCharging());

    // 4. Process Button Inputs
    buttons.update();

    ButtonEvent evLeft = buttons.getLeftEvent();
    ButtonEvent evRight = buttons.getRightEvent();
    ButtonEvent evCenter = buttons.getCenterEvent();

    if (inSettingsPage) {
        // ==========================================
        // CHE DO CAI DAT (SETTINGS MODE)
        // ==========================================
        if (inSystemInfo) {
            // Trang thong tin he thong: bam nut bat ky de quay lai danh sach
            if (evCenter != BTN_NONE || evLeft != BTN_NONE || evRight != BTN_NONE) {
                inSystemInfo = false;
                audio.playClick();
            }
        } else if (evCenter == BTN_LONG_PRESS) {
            // Nhan giu nut giua: Luu va Thoat ve man hinh HUD
            inSettingsPage = false;
            isEditingSetting = false;
            settings.save();
            applyVolume();
            display.setBrightness(settings.data.brightness);
            audio.playModeSwitch();
            display.showToast(L("Đã lưu cài đặt", "Settings saved"), 0x07E0);
            Serial.println("[SETTINGS] Da luu va thoat ve man hinh HUD.");
        } else if (evCenter == BTN_SHORT_CLICK) {
            // Nut giua: Xac nhan / Chon
            if (settingSelectedIndex == SETTING_CHECK_OTA_NOW) {
                inSettingsPage = false;
                isEditingSetting = false;
                settings.save();
                wifiOta.toggleWiFi();
                audio.playClick();
                Serial.println("[SETTINGS] Chuyen nhanh sang man hinh kiem tra OTA.");
            } else if (settingSelectedIndex == SETTING_RESET_DEFAULTS) {
                settings.resetDefaults();
                settings.save();
                applyVolume();
                display.setBrightness(settings.data.brightness);
                audio.playClick();
                display.showToast(L("Đã khôi phục gốc", "Defaults restored"), 0x07E0);
                Serial.println("[SETTINGS] Da khoi phuc cai dat goc.");
            } else if (settingSelectedIndex == SETTING_SYSTEM_INFO) {
                inSystemInfo = true;
                audio.playClick();
            } else {
                // Chuyen doi giua che do Chon muc va Chinh sua gia tri
                isEditingSetting = !isEditingSetting;
                audio.playClick();
            }
        } else if (evLeft == BTN_SHORT_CLICK) {
            if (isEditingSetting) {
                settings.adjustValue(settingSelectedIndex, false);
                if (settingSelectedIndex == SETTING_VOLUME) {
                    applyVolume();
                } else if (settingSelectedIndex == SETTING_BRIGHTNESS) {
                    display.setBrightness(settings.data.brightness);
                }
                audio.playClick();
            } else {
                if (settingSelectedIndex == 0) {
                    settingSelectedIndex = SETTING_COUNT - 1;
                } else {
                    settingSelectedIndex--;
                }
                audio.playClick();
            }
        } else if (evRight == BTN_SHORT_CLICK) {
            if (isEditingSetting) {
                settings.adjustValue(settingSelectedIndex, true);
                if (settingSelectedIndex == SETTING_VOLUME) {
                    applyVolume();
                } else if (settingSelectedIndex == SETTING_BRIGHTNESS) {
                    display.setBrightness(settings.data.brightness);
                }
                audio.playClick();
            } else {
                settingSelectedIndex = (settingSelectedIndex + 1) % SETTING_COUNT;
                audio.playClick();
            }
        }
    } else {
        // ==========================================
        // CHE DO LAI XE BINH THUONG (HUD MODE)
        // ==========================================

        // --- BUTTON BOOT (GPIO 0, Nut o giua) ---
        if (evCenter == BTN_LONG_PRESS) {
            // BAM GIU NUT GIUA: VAO TRANG CAI DAT (SETTINGS)
            inSettingsPage = true;
            isEditingSetting = false;
            audio.playModeSwitch();
            Serial.println("[BUTTON CENTER LONG] Mo trang Cai dat he thong.");
        } else if (evCenter == BTN_DOUBLE_CLICK) {
            bool wifiState = wifiOta.toggleWiFi();
            audio.playModeSwitch();
            Serial.printf("[BUTTON CENTER DOUBLE] WiFi toggled: %s\n", wifiState ? "ON (OTA)" : "OFF (HUD)");
        } else if (evCenter == BTN_SHORT_CLICK) {
            if (wifiOta.isEnabled()) {
                wifiOta.toggleWiFi();
                audio.playClick();
                Serial.println("[BUTTON] Thoat trang WiFi & OTA -> Quay lai HUD.");
            } else {
                toggleMute();
            }
        }

        // --- BUTTON VOL- (GPIO 39, Nut ben trai) ---
        // MAC DINH 1 CHAM: GIAM AM LUONG
        if (evLeft == BTN_SHORT_CLICK) {
            if (!isMuted) {
                settings.data.volume = settings.data.volume > 15 ? settings.data.volume - 15 : 0;
                settings.save();
                applyVolume();
                if (!isMuted) audio.playClick();
            }
            showVolumeToast();
            Serial.printf("[BUTTON LEFT] Giam am luong: %d%%\n", settings.data.volume);
        } else if (evLeft == BTN_LONG_PRESS) {
            toggleMute();
        } else if (evLeft == BTN_DOUBLE_CLICK) {
            speedLimit -= 10;
            if (speedLimit < 30) speedLimit = 30;
            audio.playClick();
            char toast[32];
            snprintf(toast, sizeof(toast), "%d km/h", speedLimit);
            display.showToast(toast, 0x07FF);
            Serial.printf("[BUTTON LEFT DOUBLE] Speed limit: %d km/h\n", speedLimit);
        }

        // --- BUTTON VOL+ (GPIO 40, Nut ben phai) ---
        // MAC DINH 1 CHAM: TANG AM LUONG
        if (evRight == BTN_SHORT_CLICK) {
            if (settings.data.muted && settings.data.volume > 0) {
                settings.data.muted = false;   // first press just unmutes at the saved level
            } else {
                settings.data.muted = false;
                settings.data.volume = settings.data.volume + 15 > 100 ? 100 : settings.data.volume + 15;
            }
            settings.save();
            applyVolume();
            audio.playClick();
            showVolumeToast();
            Serial.printf("[BUTTON RIGHT] Tang am luong: %d%%\n", settings.data.volume);
        } else if (evRight == BTN_LONG_PRESS) {
            display.cycleBrightness();
            settings.data.brightness = (display.getBrightness() * 100) / 255;
            settings.save();
            audio.playClick();
            Serial.println("[BUTTON RIGHT LONG] Screen brightness cycled.");
        } else if (evRight == BTN_DOUBLE_CLICK) {
            speedLimit += 10;
            if (speedLimit > 120) speedLimit = 120;
            audio.playClick();
            char toast[32];
            snprintf(toast, sizeof(toast), "%d km/h", speedLimit);
            display.showToast(toast, 0x07FF);
            Serial.printf("[BUTTON RIGHT DOUBLE] Speed limit: %d km/h\n", speedLimit);
        }
    }

    // 5. Update Speed, Route & Traffic Alerts Data (runs in background even during WiFi)
    bool currentAlertActive = false;
    uint8_t currentAlertType = 0;
    uint16_t currentAlertDist = 0;
    int effectiveLimit = speedLimit; // live GPS: a nearby alert's posted limit overrides it

    if (manualAlertActive) {
        currentAlertActive = true;
        currentAlertType = manualAlertType;
        currentAlertDist = manualAlertDist;
    } else {
        // Real GPS Mode
        if (gps.hasFix()) {
            int gpsSpeed = gps.getSpeedKmh();
            currentSpeed = settings.calibratedSpeed(gpsSpeed);
            settings.liveSpeedKmh = currentSpeed;
            float lat = gps.getLatitude();
            float lon = gps.getLongitude();
            float heading = gps.getCourse();

            roadNames.update(lat, lon, heading, gpsSpeed, now);
            const char* road = roadNames.name();
            display.setRoadName(road[0] ? road : ROAD_UNKNOWN);

            ActiveTrafficAlert alert;
            alert.active = false;
            if (settings.data.hanoiAlertsDb) {
                alert = trafficAlerts.queryNearby(lat, lon, heading, gpsSpeed);
            }
            if (alert.active) {
                currentAlertActive = true;
                currentAlertType = alert.type;
                currentAlertDist = alert.distanceM;
                if (alert.speedLimit > 0) effectiveLimit = alert.speedLimit;

                if (!isMuted && !wifiOta.isEnabled() && !inSettingsPage && settings.data.signSound && alert.distanceM <= settings.data.alertDistance &&
                    alert.index != lastAnnouncedAlertIdx && (now - lastSoundAlertTime >= 3000)) {
                    // Each point is announced once; 3 s gap so back-to-back points don't talk over each other
                    lastSoundAlertTime = now;
                    lastAnnouncedAlertIdx = alert.index;
                    if (alert.type == ALERT_SPEED_CAMERA || alert.type == ALERT_CAMERA) {
                        audio.playAlertCamera();
                    } else if (alert.type == ALERT_TRAFFIC_LIGHT) {
                        audio.playAlertTrafficLight();
                    } else if (alert.type == ALERT_RESIDENT_AREA) {
                        audio.playAlertResident();
                    } else {
                        audio.playWarn();
                    }
                }
            }
        } else {
            // No accurate position yet, but the module may already report a valid speed
            currentSpeed = settings.calibratedSpeed(gps.getSpeedKmh());
            settings.liveSpeedKmh = -1;
            display.setRoadName(ROAD_WAIT_GPS);
        }
    }

    // 6. Overspeed Audio Alert (only when not on WiFi or Settings page, and unmuted)
    int overspeedThresh = effectiveLimit + settings.data.overspeedTolerance;
    if (!isMuted && !wifiOta.isEnabled() && !inSettingsPage && settings.data.overspeedAlert && effectiveLimit > 0 && currentSpeed > overspeedThresh) {
        if (now - lastOverspeedAlertTime >= 1400) {
            lastOverspeedAlertTime = now;
            audio.playOverspeed();
        }
    }

    // 7. Render to display at ~30-40 FPS
    if (now - lastTempLogTime >= 10000) {
        lastTempLogTime = now;
        Serial.printf("[TEMP] chip=%.1fC cpu=%luMHz render=%dms\n", (double)temperatureRead(),
                      (unsigned long)getCpuFrequencyMhz(), renderIntervalMs);
    }

    if (now - lastRenderTime >= renderIntervalMs) {
        lastRenderTime = now;

        if (inSettingsPage && inSystemInfo) {
            SystemInfoData info = {
                gps.getProtocol(), gps.getBaud(), gps.getSatellites(), gps.hasFix(), gps.getHAccM(),
                trafficAlerts.getAlertCount(), roadNames.nameCount(),
                battery.isPresent(), battery.getLevel(), battery.isCharging(), gps.getTtffMs(),
            };
            display.renderSystemInfo(info);
        } else if (inSettingsPage) {
            display.renderSettings(settingSelectedIndex, isEditingSetting);
        } else {
            // Keep DisplayManager synchronized on WiFi & OTA state
            display.updateWiFiState(
                wifiOta.isEnabled(),
                wifiOta.getWorkflowState(),
                wifiOta.getWorkflowMessage(),
                wifiOta.getSSID().c_str(),
                wifiOta.getIPString(),
                wifiOta.getRSSI(),
                wifiOta.getProgress(),
                wifiOta.getCountdownSec()
            );

            display.updateData(
                currentSpeed,
                effectiveLimit,
                gps.getSatellites(),
                gps.hasFix(),
                isMuted,
                nullptr,
                wifiOta.isEnabled(),
                wifiOta.isConnected(),
                wifiOta.getIPString(),
                wifiOta.isUpdating(),
                wifiOta.getProgress(),
                currentAlertActive,
                currentAlertType,
                currentAlertDist
            );
            display.render();
        }
    }

    delay(kLoopIdleMs);   // let the idle task run (core waits for interrupt = cooler)
}
