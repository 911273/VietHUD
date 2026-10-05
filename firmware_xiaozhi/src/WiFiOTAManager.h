#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>

#define FW_VERSION "3.2.0"
#define FW_BUILD   "20261005"

enum WiFiState : uint8_t {
    WIFI_STATE_OFF = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_UPDATING
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
    uint32_t m_lastCheckTime;
    uint32_t m_lastWiFiPollTime;
    char m_ipStr[24];

    void setupWebServer();
    void startArduinoOTA();
};

extern WiFiOTAManager wifiOta;
