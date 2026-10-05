#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "HardwareConfig.h"
#include "FreeSansBold24pt7b.h"
#include "FreeSansBold18pt7b.h"
#include "SpeedSignFonts.h"

class DisplayManager {
public:
    DisplayManager();
    bool begin();

    // Update HUD data
    void updateData(
        int currentSpeed,
        int speedLimit,
        int satellites,
        bool gpsFix,
        bool isDemoMode,
        bool isMuted,
        const char* roadName = nullptr,
        bool wifiEnabled = false,
        bool wifiConnected = false,
        const char* wifiIp = nullptr,
        bool isOtaUpdating = false,
        int otaProgress = 0,
        bool alertActive = false,
        uint8_t alertType = 0,
        uint16_t alertDistance = 0
    );

    // Update WiFi & OTA dedicated screen state
    void updateWiFiState(
        bool enabled,
        uint8_t workflowState,
        const char* workflowMsg,
        const char* ssid,
        const char* ipStr,
        int rssi,
        int progress,
        int countdownSec
    );
    
    // Render frame to display (canvas flush)
    void render();

    // Set road name
    void setRoadName(const char* name);

    // Brightness controls
    void cycleBrightness();
    void setBrightness(uint8_t pct);
    uint8_t getBrightness() const { return m_brightness; }

    // Popup toast notification for user feedback
    void showToast(const char* text, uint16_t color = 0xFFFF);

    // Dedicated Full-Screen Settings Page
    void renderSettings(uint8_t selectedIdx, bool isEditing);

    // Dedicated Full-Screen OTA Progress and Result Visualizers
    void showOtaProgress(const char* title, int progress, const char* detail = nullptr);
    void showOtaSuccess(const char* version, int countdownSec = 3);
    void showOtaFailure(const char* reason);

private:
    Arduino_DataBus *m_bus;
    Arduino_GFX *m_tft;
    Arduino_Canvas *m_canvas;

    int m_speed;
    int m_speedLimit;
    int m_sats;
    bool m_fix;
    bool m_demo;
    bool m_muted;
    char m_roadName[64];

    // Marquee scrolling state
    uint32_t m_marqueeStartTime;

    // WiFi & OTA Dedicated Screen
    bool m_wifiEnabled;
    bool m_wifiConnected;
    uint8_t m_otaWorkflowState;
    char m_otaWorkflowMsg[48];
    char m_wifiSsid[32];
    char m_wifiIp[24];
    int m_wifiRssi;
    int m_otaProgress;
    int m_otaCountdownSec;

    // Traffic Alert (Icon & Distance based)
    bool m_alertActive;
    uint8_t m_alertType;
    uint16_t m_alertDistance;

    uint8_t m_brightness;
    uint8_t m_rotation;

    uint32_t m_lastFlashTime;
    bool m_flashState;

    char m_toastText[32];
    uint16_t m_toastColor;
    uint32_t m_toastExpiry;

    void drawTopBar();
    void drawAlertLeftCard();
    void drawSpeedLimitSign(int cx, int cy, int radius, int limit);
    void drawCurrentSpeedSection(int cx, int cy, int speed, int limit);
    void drawToast();
    void drawWiFiOTAPage();

    // Traffic Alert Icon Drawing Primitives (Centered at cx, cy)
    void drawIconCamera(int cx, int cy);
    void drawIconTrafficLight(int cx, int cy);
    void drawIconResidentArea(int cx, int cy);
    void drawIconNoOvertaking(int cx, int cy);
    void drawIconTollBooth(int cx, int cy);
    void drawIconDanger(int cx, int cy);
};
