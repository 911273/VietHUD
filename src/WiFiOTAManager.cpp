#include "WiFiOTAManager.h"

WiFiOTAManager wifiOta;

static const char* HTML_UPLOAD = 
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>VietHUD OTA Update</title>"
"<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;padding:20px}"
".card{background:#222;border:1px solid #444;border-radius:10px;padding:20px;max-width:400px;margin:auto}"
"input[type=file]{margin:15px 0}button{background:#00d26a;color:#fff;border:none;padding:10px 20px;border-radius:5px;font-size:16px;cursor:pointer}</style></head>"
"<body><div class='card'><h2>VietHUD Lite OTA</h2><p>Phien ban hien tai: " FW_VERSION "</p>"
"<form method='POST' action='/update' enctype='multipart/form-data'>"
"<input type='file' name='update' accept='.bin'><br><button type='submit'>Nap Firmware (.bin)</button></form></div></body></html>";

WiFiOTAManager::WiFiOTAManager()
    : m_server(80), m_enabled(false), m_state(WIFI_STATE_OFF),
      m_isUpdating(false), m_progress(0), m_lastWiFiPollTime(0),
      m_workflowState(OTA_STEP_IDLE), m_stateChangeTime(0), m_autoReturnDurationMs(6000) {
    m_ipStr[0] = '\0';
    m_workflowMsg[0] = '\0';
    m_remoteVersion[0] = '\0';
}

void WiFiOTAManager::begin() {
    // Register the 2 requested default WiFi APs
    m_wifiMulti.addAP("iPhone của Pham", "12345678@");
    m_wifiMulti.addAP("VuPQ", "moth@ib@bon");

    WiFi.mode(WIFI_OFF);
    m_state = WIFI_STATE_OFF;
    m_workflowState = OTA_STEP_IDLE;
}

bool WiFiOTAManager::toggleWiFi() {
    m_enabled = !m_enabled;

    if (m_enabled) {
        Serial.println("[WIFI] Bat WiFi -> Chuyen sang trang KET NOI & CAP NHAT OTA.");
        WiFi.mode(WIFI_STA);
        m_state = WIFI_STATE_CONNECTING;
        m_workflowState = OTA_STEP_CONNECTING_WIFI;
        m_stateChangeTime = millis();
        m_progress = 0;
        m_ipStr[0] = '\0';
        m_remoteVersion[0] = '\0';
        snprintf(m_workflowMsg, sizeof(m_workflowMsg), "DANG KET NOI WIFI...");
        m_wifiMulti.run();
        m_lastWiFiPollTime = millis();
    } else {
        Serial.println("[WIFI] Tat WiFi -> Quay lai trang HUD.");
        ArduinoOTA.end();
        m_server.stop();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        m_state = WIFI_STATE_OFF;
        m_workflowState = OTA_STEP_IDLE;
        m_ipStr[0] = '\0';
    }

    return m_enabled;
}

String WiFiOTAManager::getSSID() const {
    if (m_state == WIFI_STATE_CONNECTED) {
        return WiFi.SSID();
    }
    return String("");
}

int WiFiOTAManager::getRSSI() const {
    if (m_state == WIFI_STATE_CONNECTED) {
        return WiFi.RSSI();
    }
    return 0;
}

int WiFiOTAManager::getCountdownSec() const {
    if (m_workflowState == OTA_STEP_UP_TO_DATE || m_workflowState == OTA_STEP_FAILED) {
        uint32_t elapsed = millis() - m_stateChangeTime;
        if (elapsed < m_autoReturnDurationMs) {
            return (int)((m_autoReturnDurationMs - elapsed) / 1000) + 1;
        }
        return 0;
    }
    return 0;
}

void WiFiOTAManager::startArduinoOTA() {
    ArduinoOTA.setHostname("viethud-lite");

    ArduinoOTA.onStart([this]() {
        m_workflowState = OTA_STEP_DOWNLOADING;
        m_isUpdating = true;
        m_progress = 0;
        snprintf(m_workflowMsg, sizeof(m_workflowMsg), "DANG NAP ARDUINO OTA...");
        Serial.println("[OTA] Bat dau qua trinh cap nhat ArduinoOTA...");
    });

    ArduinoOTA.onEnd([this]() {
        m_workflowState = OTA_STEP_REBOOTING;
        m_isUpdating = false;
        m_progress = 100;
        snprintf(m_workflowMsg, sizeof(m_workflowMsg), "CAP NHAT XONG! REBOOT...");
        Serial.println("[OTA] Cap nhat thanh cong! Khoi dong lai...");
    });

    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        m_progress = (progress * 100) / total;
    });

    ArduinoOTA.onError([this](ota_error_t error) {
        m_workflowState = OTA_STEP_FAILED;
        m_stateChangeTime = millis();
        m_autoReturnDurationMs = 8000;
        snprintf(m_workflowMsg, sizeof(m_workflowMsg), "LOI ARDUINO OTA (%u)", error);
        Serial.printf("[OTA ERROR] Loi ma: %u\n", error);
    });

    ArduinoOTA.begin();
    Serial.println("[OTA] ArduinoOTA da san sang (hostname: viethud-lite.local)");
}

void WiFiOTAManager::setupWebServer() {
    m_server.on("/", HTTP_GET, [this]() {
        m_server.send(200, "text/html", HTML_UPLOAD);
    });

    m_server.on("/update", HTTP_POST, [this]() {
        m_server.sendHeader("Connection", "close");
        m_server.send(200, "text/plain", (Update.hasError()) ? "CAP NHAT THAT BAI" : "THANH CONG. DANG KHOI DONG LAI...");
        delay(800);
        ESP.restart();
    }, [this]() {
        HTTPUpload& upload = m_server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            m_workflowState = OTA_STEP_DOWNLOADING;
            m_isUpdating = true;
            m_progress = 0;
            snprintf(m_workflowMsg, sizeof(m_workflowMsg), "DANG NAP WEB OTA...");
            Serial.printf("[WEB OTA] Bat dau nap file: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
            if (upload.totalSize > 0) {
                m_progress = (upload.currentSize * 100) / upload.totalSize;
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                m_progress = 100;
                m_workflowState = OTA_STEP_REBOOTING;
                snprintf(m_workflowMsg, sizeof(m_workflowMsg), "NAP XONG! REBOOT...");
                Serial.printf("[WEB OTA] Hoan tat! Kich thuoc: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
                m_workflowState = OTA_STEP_FAILED;
                m_stateChangeTime = millis();
                m_autoReturnDurationMs = 8000;
                snprintf(m_workflowMsg, sizeof(m_workflowMsg), "LOI GHI FLASH WEB OTA");
            }
            m_isUpdating = false;
        }
    });

    m_server.begin();
    Serial.printf("[WEB OTA] Web server OTA da chay tai: http://%s/\n", m_ipStr);
}

void WiFiOTAManager::executeHttpCheck() {
    Serial.println("[OTA] Dang kiem tra version.json tu Pi 4 (192.168.1.65)...");

    bool hasUpdate = false;
    String targetUrl = "";
    String remoteVer = "";
    String remoteBuild = "";

    HTTPClient http;
    http.setTimeout(4500);

    // Try Pi 4 LAN IP
    if (http.begin("http://192.168.1.65/viethud/firmware/version.json")) {
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            String payload = http.getString();
            Serial.println("[OTA] Nhan version.json:");
            Serial.println(payload);

            int vIdx = payload.indexOf("\"version\":");
            if (vIdx > 0) {
                int q1 = payload.indexOf("\"", vIdx + 10);
                int q2 = payload.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) remoteVer = payload.substring(q1 + 1, q2);
            }

            int uIdx = payload.indexOf("\"url_pi4\":");
            if (uIdx > 0) {
                int q1 = payload.indexOf("\"", uIdx + 10);
                int q2 = payload.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) targetUrl = payload.substring(q1 + 1, q2);
            }

            int bIdx = payload.indexOf("\"build\":");
            if (bIdx > 0) {
                int q1 = payload.indexOf("\"", bIdx + 8);
                int q2 = payload.indexOf("\"", q1 + 1);
                if (q1 > 0 && q2 > q1) remoteBuild = payload.substring(q1 + 1, q2);
            }
        }
        http.end();
    }

    if (remoteVer.length() > 0) {
        strncpy(m_remoteVersion, remoteVer.c_str(), sizeof(m_remoteVersion) - 1);
        if (remoteVer != FW_VERSION || remoteBuild != FW_BUILD) {
            hasUpdate = true;
        }
    }

    if (hasUpdate && targetUrl.length() > 0) {
        m_workflowState = OTA_STEP_DOWNLOADING;
        m_stateChangeTime = millis();
        m_isUpdating = true;
        m_progress = 5;
        snprintf(m_workflowMsg, sizeof(m_workflowMsg), "DANG TAI FIRMWARE %s...", remoteVer.c_str());
        Serial.printf("[OTA] Tim thay firmware moi: %s tai %s\n", remoteVer.c_str(), targetUrl.c_str());

        httpUpdate.setLedPin(-1);
        httpUpdate.onProgress([this](int current, int total) {
            if (total > 0) {
                m_progress = (current * 100) / total;
            }
        });

        WiFiClient client;
        t_httpUpdate_return ret = httpUpdate.update(client, targetUrl);
        m_isUpdating = false;

        if (ret == HTTP_UPDATE_OK) {
            m_workflowState = OTA_STEP_REBOOTING;
            snprintf(m_workflowMsg, sizeof(m_workflowMsg), "CAP NHAT XONG! REBOOT...");
            delay(1200);
            ESP.restart();
        } else {
            m_workflowState = OTA_STEP_FAILED;
            m_stateChangeTime = millis();
            m_autoReturnDurationMs = 8000;
            snprintf(m_workflowMsg, sizeof(m_workflowMsg), "LOI HTTP UPDATE (%d)", httpUpdate.getLastError());
        }
    } else if (remoteVer.length() > 0) {
        // Up to date!
        m_workflowState = OTA_STEP_UP_TO_DATE;
        m_stateChangeTime = millis();
        m_autoReturnDurationMs = 6000;
        snprintf(m_workflowMsg, sizeof(m_workflowMsg), "HE THONG MOI NHAT (%s)", FW_VERSION);
        Serial.println("[OTA] Thiet bi dang o phien ban moi nhat!");
    } else {
        // Could not reach server
        m_workflowState = OTA_STEP_FAILED;
        m_stateChangeTime = millis();
        m_autoReturnDurationMs = 8000;
        snprintf(m_workflowMsg, sizeof(m_workflowMsg), "KHONG KET NOI DUOC PI 4");
        Serial.println("[OTA] Khong the tai version.json tu Pi 4.");
    }
}

bool WiFiOTAManager::checkForUpdate() {
    if (m_state != WIFI_STATE_CONNECTED) return false;
    executeHttpCheck();
    return true;
}

void WiFiOTAManager::update() {
    if (!m_enabled) return;

    uint32_t now = millis();

    switch (m_workflowState) {
        case OTA_STEP_CONNECTING_WIFI: {
            if (WiFi.status() == WL_CONNECTED) {
                m_state = WIFI_STATE_CONNECTED;
                IPAddress ip = WiFi.localIP();
                snprintf(m_ipStr, sizeof(m_ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
                m_workflowState = OTA_STEP_WIFI_CONNECTED;
                m_stateChangeTime = now;
                snprintf(m_workflowMsg, sizeof(m_workflowMsg), "DA KET NOI: %s", WiFi.SSID().c_str());
                Serial.printf("[WIFI] %s, IP: %s\n", m_workflowMsg, m_ipStr);
                startArduinoOTA();
                setupWebServer();
            } else {
                if (now - m_lastWiFiPollTime >= 1000) {
                    m_lastWiFiPollTime = now;
                    m_wifiMulti.run();
                }
                // Timeout after 20 seconds
                if (now - m_stateChangeTime >= 20000) {
                    m_workflowState = OTA_STEP_FAILED;
                    m_stateChangeTime = now;
                    m_autoReturnDurationMs = 8000;
                    snprintf(m_workflowMsg, sizeof(m_workflowMsg), "KHONG TIM THAY WIFI");
                    Serial.println("[WIFI] Ket noi WiFi that bai sau 20s.");
                }
            }
            break;
        }

        case OTA_STEP_WIFI_CONNECTED: {
            ArduinoOTA.handle();
            m_server.handleClient();

            // Wait 1.5s so user can read connected info, then check update automatically
            if (now - m_stateChangeTime >= 1500) {
                m_workflowState = OTA_STEP_CHECKING_UPDATE;
                m_stateChangeTime = now;
                snprintf(m_workflowMsg, sizeof(m_workflowMsg), "DANG KIEM TRA PHIEN BAN...");
                Serial.println("[WIFI] Dang kiem tra version.json...");
            }
            break;
        }

        case OTA_STEP_CHECKING_UPDATE: {
            ArduinoOTA.handle();
            m_server.handleClient();
            executeHttpCheck();
            break;
        }

        case OTA_STEP_UP_TO_DATE: {
            ArduinoOTA.handle();
            m_server.handleClient();

            if (now - m_stateChangeTime >= m_autoReturnDurationMs) {
                Serial.println("[WIFI] Het thoi gian cho. Tu dong quay lai HUD...");
                toggleWiFi();
            }
            break;
        }

        case OTA_STEP_FAILED: {
            if (now - m_stateChangeTime >= m_autoReturnDurationMs) {
                Serial.println("[WIFI] Tu dong quay lai HUD sau khi that bai...");
                toggleWiFi();
            }
            break;
        }

        default:
            if (WiFi.status() == WL_CONNECTED) {
                ArduinoOTA.handle();
                m_server.handleClient();
            }
            break;
    }
}
