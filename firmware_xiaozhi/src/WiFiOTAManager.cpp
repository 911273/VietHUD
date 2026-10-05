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
      m_isUpdating(false), m_progress(0), m_lastCheckTime(0), m_lastWiFiPollTime(0) {
    m_ipStr[0] = '\0';
}

void WiFiOTAManager::begin() {
    // Register the 2 requested default WiFi APs
    m_wifiMulti.addAP("iPhone của Pham", "12345678@");
    m_wifiMulti.addAP("VuPQ", "moth@ib@bon");

    // Make sure WiFi radio is initially OFF to save power & reduce RF interference
    WiFi.mode(WIFI_OFF);
    m_state = WIFI_STATE_OFF;
}

bool WiFiOTAManager::toggleWiFi() {
    m_enabled = !m_enabled;

    if (m_enabled) {
        Serial.println("[WIFI] Bat WiFi. Dang tim kiem mang (iPhone cua Pham / VuPQ)...");
        WiFi.mode(WIFI_STA);
        m_state = WIFI_STATE_CONNECTING;
        m_wifiMulti.run();
        m_lastWiFiPollTime = millis();
    } else {
        Serial.println("[WIFI] Tat WiFi.");
        ArduinoOTA.end();
        m_server.stop();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        m_state = WIFI_STATE_OFF;
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

void WiFiOTAManager::startArduinoOTA() {
    ArduinoOTA.setHostname("viethud-lite");

    ArduinoOTA.onStart([this]() {
        m_isUpdating = true;
        m_progress = 0;
        Serial.println("[OTA] Bat dau qua trinh cap nhat ArduinoOTA...");
    });

    ArduinoOTA.onEnd([this]() {
        m_isUpdating = false;
        m_progress = 100;
        Serial.println("[OTA] Cap nhat thanh cong! Khoi dong lai...");
    });

    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        m_progress = (progress * 100) / total;
        Serial.printf("[OTA] Tien do: %u%%\r", m_progress);
    });

    ArduinoOTA.onError([](ota_error_t error) {
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
            m_isUpdating = true;
            m_progress = 0;
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
                Serial.printf("[WEB OTA] Hoan tat! Kich thuoc: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
            m_isUpdating = false;
        }
    });

    m_server.begin();
    Serial.printf("[WEB OTA] Web server OTA da chay tai: http://%s/\n", m_ipStr);
}

bool WiFiOTAManager::checkForUpdate() {
    if (m_state != WIFI_STATE_CONNECTED) return false;

    Serial.println("[OTA] Kiem tra ban cap nhat tren Pi 4 (http://192.168.1.65/viethud/firmware/version.json)...");

    HTTPClient http;
    http.setTimeout(4000);

    // 1. Try Pi 4 LAN IP first
    bool found = false;
    String targetUrl = "";

    if (http.begin("http://192.168.1.65/viethud/firmware/version.json")) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            Serial.println("[OTA] Nhan version.json tu Pi 4:");
            Serial.println(payload);

            // Simple parse of "url_pi4": "..."
            int urlIdx = payload.indexOf("\"url_pi4\":");
            if (urlIdx > 0) {
                int startQuote = payload.indexOf("\"", urlIdx + 10);
                int endQuote = payload.indexOf("\"", startQuote + 1);
                if (startQuote > 0 && endQuote > startQuote) {
                    targetUrl = payload.substring(startQuote + 1, endQuote);
                    found = true;
                }
            }
        }
        http.end();
    }

    if (!found) {
        Serial.println("[OTA] Khong ket noi duoc Pi 4 qua IP 192.168.1.65.");
        return false;
    }

    Serial.printf("[OTA] Tim thay URL firmware tren Pi 4: %s\n", targetUrl.c_str());
    Serial.println("[OTA] Bat dau tai va nap firmware...");

    m_isUpdating = true;
    m_progress = 10;

    // Use httpUpdate to download & flash
    httpUpdate.setLedPin(-1);
    httpUpdate.onProgress([this](int current, int total) {
        if (total > 0) {
            m_progress = (current * 100) / total;
            Serial.printf("[OTA] Downloading: %d%%\r", m_progress);
        }
    });

    WiFiClient client;
    t_httpUpdate_return ret = httpUpdate.update(client, targetUrl);

    m_isUpdating = false;
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            Serial.printf("[OTA ERROR] HTTP Update that bai! Loi (%d): %s\n",
                          httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            return false;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("[OTA] Firmware da la ban moi nhat.");
            return true;
        case HTTP_UPDATE_OK:
            Serial.println("[OTA] Cap nhat thanh cong! Khoi dong lai...");
            ESP.restart();
            return true;
    }

    return false;
}

void WiFiOTAManager::update() {
    if (!m_enabled) return;

    uint32_t now = millis();

    // 1. Maintain WiFi connection
    if (WiFi.status() == WL_CONNECTED) {
        if (m_state != WIFI_STATE_CONNECTED) {
            m_state = WIFI_STATE_CONNECTED;
            IPAddress ip = WiFi.localIP();
            snprintf(m_ipStr, sizeof(m_ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            Serial.printf("[WIFI] Da ket noi thanh cong! SSID: %s, IP: %s, RSSI: %d dBm\n",
                          WiFi.SSID().c_str(), m_ipStr, WiFi.RSSI());

            // Initialize ArduinoOTA and Web Server
            startArduinoOTA();
            setupWebServer();
        }

        // Handle active OTA requests
        ArduinoOTA.handle();
        m_server.handleClient();
    } else {
        if (m_state == WIFI_STATE_CONNECTED) {
            m_state = WIFI_STATE_DISCONNECTED;
            Serial.println("[WIFI] Mat ket noi WiFi. Dang thu ket noi lai...");
        }

        // Poll WiFiMulti every 1.5s
        if (now - m_lastWiFiPollTime >= 1500) {
            m_lastWiFiPollTime = now;
            m_wifiMulti.run();
        }
    }
}
