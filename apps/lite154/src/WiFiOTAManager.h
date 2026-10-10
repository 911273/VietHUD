#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>

#define FW_VERSION "3.4.1"
#define FW_BUILD   "20261005_v3.4.1"
#define PI4_LAN_IP          "192.168.1.65"
#define PI4_TAILSCALE_IP    "100.107.34.92"

enum WiFiState : uint8_t {
    WIFI_STATE_OFF = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_UPDATING
};

enum OTAWorkflowState : uint8_t {
    OTA_STEP_IDLE = 0,
    OTA_STEP_CONNECTING_WIFI,    // 1: Connecting to WiFi
    OTA_STEP_WIFI_CONNECTED,      // 2: Connected, showing IP/SSID
    OTA_STEP_CHECKING_UPDATE,    // 3: Checking version.json from Pi 4 / GitHub
    OTA_STEP_DOWNLOADING,        // 4: Downloading & flashing OTA
    OTA_STEP_UP_TO_DATE,         // 5: Already up to date
    OTA_STEP_FAILED,             // 6: Failed or timeout
    OTA_STEP_REBOOTING           // 7: Rebooting
};

class WiFiOTAManager {
public:
    WiFiOTAManager();
    void begin();
    bool toggleWiFi(); // Returns new state (true=ON, false=OFF)
    void update();     // Call frequently in main loop()

    bool isEnabled() const { return m_enabled; }
    bool isConnected() const { return m_state == WIFI_STATE_CONNECTED; }
    bool isUpdating() const { return m_isUpdating; }
    int getProgress() const { return m_progress; }
    WiFiState getState() const { return m_state; }

    OTAWorkflowState getWorkflowState() const { return m_workflowState; }
    const char* getWorkflowMessage() const { return m_workflowMsg; }
    const char* getRemoteVersion() const { return m_remoteVersion; }
    int getCountdownSec() const;

    const char* getIPString() const { return m_ipStr; }
    String getSSID() const;
    int getRSSI() const;

    // Trigger HTTP OTA check against Pi 4 / GitHub
    bool checkForUpdate();

private:
    WiFiMulti m_wifiMulti;
    WebServer m_server;
    bool m_enabled;
    WiFiState m_state;
    bool m_isUpdating;
    int m_progress;
    uint32_t m_lastWiFiPollTime;
    char m_ipStr[24];

    // OTA Workflow state machine
    OTAWorkflowState m_workflowState;
    char m_workflowMsg[48];
    char m_remoteVersion[16];
    uint32_t m_stateChangeTime;
    uint32_t m_autoReturnDurationMs;

    void setupWebServer();
    void startArduinoOTA();
    void executeHttpCheck();
};

extern WiFiOTAManager wifiOta;
