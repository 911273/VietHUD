#include <Arduino.h>
#include "HardwareConfig.h"
#include "DisplayManager.h"
#include "GPSManager.h"
#include "ButtonManager.h"
#include "AudioManager.h"
#include "WiFiOTAManager.h"
#include "TrafficAlertManager.h"
#include "SettingsManager.h"

DisplayManager display;
GPSManager gps;
ButtonManager buttons;

// State variables
int currentSpeed = 0;
int speedLimit = 60;
bool isDemoMode = true;
bool isMuted = false;

// Settings state
bool inSettingsPage = false;
bool isEditingSetting = false;
uint8_t settingSelectedIndex = 0;

// Demo scenarios: realistic Hanoi & 200km routes with real alerts
struct DemoRoute {
    const char* roadName;
    int limit;
    float maxSpeed;
    float minSpeed;
    float lat;
    float lon;
    float heading;
    uint8_t alertType;
};

const DemoRoute HANOI_DEMO_ROUTES[] = {
    {
        "DUONG CAO TOC NOI BAI - LAO CAI",
        100, 108.0f, 75.0f,
        21.2354f, 105.7820f, 320.0f,
        ALERT_SPEED_CAMERA
    },
    {
        "DUONG VANH DAI 3 TREN CAO (TRUNG HOA)",
        80, 86.0f, 55.0f,
        21.0082f, 105.7951f, 45.0f,
        ALERT_CAMERA
    },
    {
        "DAI LO THANG LONG (HA NOI)",
        100, 105.0f, 80.0f,
        21.0025f, 105.7420f, 265.0f,
        ALERT_NO_OVERTAKING
    },
    {
        "DUONG VO NGUYEN GIAP (SAN BAY)",
        80, 85.0f, 50.0f,
        21.1850f, 105.8190f, 355.0f,
        ALERT_SPEED_CAMERA
    },
    {
        "DUONG GIAI PHONG - NGOC HOI",
        60, 64.0f, 35.0f,
        20.9710f, 105.8450f, 180.0f,
        ALERT_RESIDENT_AREA
    },
    {
        "DUONG PHAM VAN DONG (CO NHUE)",
        60, 65.0f, 30.0f,
        21.0620f, 105.7830f, 0.0f,
        ALERT_TRAFFIC_LIGHT
    },
    {
        "QUOC LO 5 (HA NOI - HAI PHONG)",
        80, 84.0f, 52.0f,
        21.0050f, 105.9520f, 95.0f,
        ALERT_TOLL_BOOTH
    },
    {
        "DUONG NGUYEN TRAI - HA DONG",
        50, 56.0f, 25.0f,
        20.9850f, 105.7920f, 230.0f,
        ALERT_TRAFFIC_LIGHT
    }
};
const int NUM_DEMO_ROUTES = sizeof(HANOI_DEMO_ROUTES) / sizeof(HANOI_DEMO_ROUTES[0]);
int currentRouteIdx = 0;

// Simulation state
float demoSpeedFloat = 55.0f;
float demoAcceleration = 1.3f;
int demoAlertDist = 480;
uint32_t lastSimTime = 0;
uint32_t lastRouteSwitchTime = 0;
uint32_t lastRenderTime = 0;
uint32_t lastOverspeedAlertTime = 0;
uint32_t lastSoundAlertTime = 0;

// Manual alert override via Serial for testing
bool manualAlertActive = false;
uint8_t manualAlertType = 0;
uint16_t manualAlertDist = 0;

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
        } else if (line.equalsIgnoreCase("DEMO")) {
            isDemoMode = true;
            display.showToast("DEMO", 0xFD20);
            Serial.println("[CMD] Che do DEMO");
        } else if (line.equalsIgnoreCase("GPS")) {
            isDemoMode = false;
            display.showToast("GPS", 0x07FF);
            Serial.println("[CMD] Che do GPS");
        } else if (line.equalsIgnoreCase("WIFI")) {
            bool wifiState = wifiOta.toggleWiFi();
            Serial.printf("[CMD] WiFi: %s\n", wifiState ? "ON (CHUYEN TRANG KET NOI & OTA)" : "OFF (QUAY VE HUD)");
        } else if (line.equalsIgnoreCase("OTA")) {
            Serial.println("[CMD] Kiem tra cap nhat OTA tu Pi 4...");
            display.showToast("CHECK OTA...", 0x07FF);
            wifiOta.checkForUpdate();
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("=========================================");
    Serial.println("   VIETHUD LITE v3.3.0 - HANOI 200KM     ");
    Serial.println("=========================================");

    // 0. Initialize Settings from NVS flash
    settings.begin();
    audio.setVolume(settings.data.volume);
    display.setBrightness(settings.data.brightness);
    isDemoMode = settings.data.demoMode;
    speedLimit = settings.data.defaultSpeedLimit;

    // 1. Initialize display & hardware
    if (!display.begin()) {
        Serial.println("[ERROR] Display initialization failed!");
    } else {
        Serial.println("[OK] Display initialized successfully.");
        display.setBrightness(settings.data.brightness);
    }

    // Set initial Hanoi route and limit
    speedLimit = HANOI_DEMO_ROUTES[0].limit;
    display.setRoadName(HANOI_DEMO_ROUTES[0].roadName);

    // 2. Initialize Audio I2S
    if (audio.begin()) {
        Serial.println("[OK] Audio I2S initialized on DOUT=7, BCLK=15, LRCK=16.");
        audio.setVolume(settings.data.volume);
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

    lastSimTime = millis();
    lastRouteSwitchTime = millis();
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

    // 4. Process Button Inputs
    buttons.update();

    ButtonEvent evLeft = buttons.getLeftEvent();
    ButtonEvent evRight = buttons.getRightEvent();
    ButtonEvent evCenter = buttons.getCenterEvent();

    if (inSettingsPage) {
        // ==========================================
        // CHE DO CAI DAT (SETTINGS MODE)
        // ==========================================
        if (evCenter == BTN_LONG_PRESS) {
            // Nhan giu nut giua: Luu va Thoat ve man hinh HUD
            inSettingsPage = false;
            isEditingSetting = false;
            settings.save();
            audio.setVolume(settings.data.volume);
            display.setBrightness(settings.data.brightness);
            audio.playModeSwitch();
            display.showToast("DA LUU CAI DAT!", 0x07E0);
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
                audio.setVolume(settings.data.volume);
                display.setBrightness(settings.data.brightness);
                audio.playClick();
                display.showToast("DA KHOI PHUC!", 0x07E0);
                Serial.println("[SETTINGS] Da khoi phuc cai dat goc.");
            } else if (settingSelectedIndex == SETTING_SYSTEM_INFO) {
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
                    audio.setVolume(settings.data.volume);
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
                    audio.setVolume(settings.data.volume);
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
                isDemoMode = !isDemoMode;
                audio.playModeSwitch();
                if (isDemoMode) {
                    display.showToast("DEMO", 0xFD20);
                } else {
                    display.showToast("GPS", 0x07FF);
                    display.setRoadName("GPS LIVE");
                }
                Serial.printf("[BUTTON] Mode switched: %s\n", isDemoMode ? "DEMO" : "LIVE GPS");
            }
        }

        // --- BUTTON VOL- (GPIO 39, Nut ben trai) ---
        // MAC DINH 1 CHAM: GIAM AM LUONG
        if (evLeft == BTN_SHORT_CLICK) {
            uint8_t curVol = audio.volumeDown(15);
            settings.data.volume = curVol;
            settings.save();
            if (curVol == 0) {
                isMuted = true;
                display.showToast("MUTE", 0xF800);
            } else {
                isMuted = false;
                audio.playClick();
                char toast[32];
                snprintf(toast, sizeof(toast), "AM LUONG: %d%%", curVol);
                display.showToast(toast, 0x07E0);
            }
            Serial.printf("[BUTTON LEFT] Giam am luong: %d%%\n", curVol);
        } else if (evLeft == BTN_LONG_PRESS) {
            isMuted = !isMuted;
            if (isMuted) {
                audio.setVolume(0);
                display.showToast("MUTE", 0xF800);
            } else {
                audio.setVolume(settings.data.volume);
                audio.playClick();
                char toast[32];
                snprintf(toast, sizeof(toast), "AM LUONG: %d%%", settings.data.volume);
                display.showToast(toast, 0x07E0);
            }
            Serial.printf("[BUTTON LEFT LONG] Mute: %s\n", isMuted ? "MUTED" : "UNMUTED");
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
            uint8_t curVol = audio.volumeUp(15);
            settings.data.volume = curVol;
            settings.save();
            isMuted = false;
            audio.playClick();
            char toast[32];
            snprintf(toast, sizeof(toast), "AM LUONG: %d%%", curVol);
            display.showToast(toast, 0x07E0);
            Serial.printf("[BUTTON RIGHT] Tang am luong: %d%%\n", curVol);
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

    if (manualAlertActive) {
        currentAlertActive = true;
        currentAlertType = manualAlertType;
        currentAlertDist = manualAlertDist;
    } else if (isDemoMode) {
        // Cycle Hanoi demo routes every 9 seconds
        if (now - lastRouteSwitchTime >= 9000) {
            lastRouteSwitchTime = now;
            currentRouteIdx = (currentRouteIdx + 1) % NUM_DEMO_ROUTES;
            speedLimit = HANOI_DEMO_ROUTES[currentRouteIdx].limit;
            display.setRoadName(HANOI_DEMO_ROUTES[currentRouteIdx].roadName);
            demoAlertDist = 480;

            uint8_t aType = HANOI_DEMO_ROUTES[currentRouteIdx].alertType;
            if (!isMuted && !wifiOta.isEnabled()) {
                if (aType == ALERT_SPEED_CAMERA || aType == ALERT_CAMERA) {
                    audio.playAlertCamera();
                } else if (aType == ALERT_TRAFFIC_LIGHT) {
                    audio.playAlertTrafficLight();
                } else if (aType == ALERT_RESIDENT_AREA) {
                    audio.playAlertResident();
                } else {
                    audio.playWarn();
                }
            }

            Serial.printf("[DEMO] Tuyen duong: %s (Limit: %d km/h, Alert Type: %d)\n",
                          HANOI_DEMO_ROUTES[currentRouteIdx].roadName,
                          speedLimit,
                          aType);
        }

        // Smooth speed simulation
        if (now - lastSimTime >= 50) {
            lastSimTime = now;
            float maxSpd = HANOI_DEMO_ROUTES[currentRouteIdx].maxSpeed;
            float minSpd = HANOI_DEMO_ROUTES[currentRouteIdx].minSpeed;

            demoSpeedFloat += demoAcceleration;
            if (demoSpeedFloat >= maxSpd) {
                demoSpeedFloat = maxSpd;
                demoAcceleration = -1.5f;
            } else if (demoSpeedFloat <= minSpd) {
                demoSpeedFloat = minSpd;
                demoAcceleration = 1.2f;
            }
            currentSpeed = (int)round(demoSpeedFloat);

            demoAlertDist -= 3;
            if (demoAlertDist < 40) demoAlertDist = 40;
        }

        currentAlertActive = true;
        currentAlertType = HANOI_DEMO_ROUTES[currentRouteIdx].alertType;
        currentAlertDist = demoAlertDist;

    } else {
        // Real GPS Mode
        if (gps.hasFix()) {
            currentSpeed = gps.getSpeedKmh();
            float lat = gps.getLatitude();
            float lon = gps.getLongitude();
            float heading = gps.getCourse();

            ActiveTrafficAlert alert = trafficAlerts.queryNearby(lat, lon, heading, currentSpeed);
            if (alert.active) {
                currentAlertActive = true;
                currentAlertType = alert.type;
                currentAlertDist = alert.distanceM;

                if (!isMuted && !wifiOta.isEnabled() && !inSettingsPage && settings.data.signSound && alert.distanceM <= settings.data.alertDistance && (now - lastSoundAlertTime >= 12000)) {
                    lastSoundAlertTime = now;
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
            currentSpeed = 0;
        }
    }

    // 6. Overspeed Audio Alert (only when not on WiFi or Settings page, and unmuted)
    int overspeedThresh = speedLimit + settings.data.overspeedTolerance;
    if (!isMuted && !wifiOta.isEnabled() && !inSettingsPage && settings.data.overspeedAlert && speedLimit > 0 && currentSpeed > overspeedThresh) {
        if (now - lastOverspeedAlertTime >= 1400) {
            lastOverspeedAlertTime = now;
            audio.playOverspeed();
        }
    }

    // 7. Render to display at ~30-40 FPS
    if (now - lastRenderTime >= 25) {
        lastRenderTime = now;

        if (inSettingsPage) {
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
                speedLimit,
                gps.getSatellites(),
                gps.hasFix(),
                isDemoMode,
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
}
