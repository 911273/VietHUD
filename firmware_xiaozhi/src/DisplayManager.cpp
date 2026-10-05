#include "DisplayManager.h"
#include <math.h>

// Curated Automotive Palette (RGB565)
#define COLOR_BG          0x0000 // Deep OLED Black
#define COLOR_CARD_BG     0x0842 // Dark Slate Navy Card
#define COLOR_CARD_BORDER 0x29E8 // Subtle Steel Blue
#define COLOR_TRACK       0x2124 // Dark Charcoal
#define COLOR_SAFE        0x07E0 // Bright Emerald Green (Safe)
#define COLOR_WARN        0xFD20 // Vivid Amber / Orange (Warning)
#define COLOR_ALERT       0xF800 // High-Visibility Bright Red (Danger)
#define COLOR_WHITE       0xFFFF
#define COLOR_BLACK       0x0000
#define COLOR_CYAN        0x07FF
#define COLOR_ROAD_TEXT   0xFFE0 // Crisp Amber-Yellow for Road Name
#define COLOR_SILVER      0xBDF7
#define COLOR_SIGN_RED    0xD800 // Official Vietnam Traffic Sign Red
#define COLOR_TOAST_BG    0x18E3

DisplayManager::DisplayManager()
    : m_bus(nullptr), m_tft(nullptr), m_canvas(nullptr),
      m_speed(0), m_speedLimit(60), m_sats(0), m_fix(false), m_demo(true), m_muted(false),
      m_marqueeStartTime(0),
      m_wifiEnabled(false), m_wifiConnected(false),
      m_otaWorkflowState(0), m_wifiRssi(0), m_otaProgress(0), m_otaCountdownSec(0),
      m_alertActive(false), m_alertType(0), m_alertDistance(0),
      m_brightness(255), m_rotation(0),
      m_lastFlashTime(0), m_flashState(false),
      m_toastColor(COLOR_WHITE), m_toastExpiry(0) {
    m_toastText[0] = '\0';
    m_wifiIp[0] = '\0';
    m_wifiSsid[0] = '\0';
    m_otaWorkflowMsg[0] = '\0';
    strncpy(m_roadName, "DUONG CAO TOC NOI BAI - LAO CAI", sizeof(m_roadName) - 1);
    m_roadName[sizeof(m_roadName) - 1] = '\0';
}

bool DisplayManager::begin() {
    // 1. Power Hold (GPIO 21 MUST be HIGH to keep board & display powered)
    pinMode(PIN_POWER_HOLD, OUTPUT);
    digitalWrite(PIN_POWER_HOLD, HIGH);

    // 2. Initialize Backlight
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);

    // Set other candidate pins to HIGH for safety
    pinMode(2, OUTPUT);
    digitalWrite(2, HIGH);

    // 3. Initialize SPI bus for ST7789 (DC=8, CS=14, SCK=9, MOSI=10)
    m_bus = new Arduino_ESP32SPI(
        PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, GFX_NOT_DEFINED
    );

    // 4. Initialize ST7789 (240x240, IPS=true, RST=18)
    m_tft = new Arduino_ST7789(
        m_bus, PIN_LCD_RST, m_rotation, true /* IPS */,
        LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0
    );

    if (!m_tft->begin()) {
        return false;
    }

    // 5. Initialize Canvas for smooth flicker-free rendering
    m_canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, m_tft);
    if (!m_canvas) {
        return false;
    }

    if (!m_canvas->begin()) {
        return false;
    }

    m_canvas->fillScreen(COLOR_BG);
    m_canvas->flush();
    return true;
}

void DisplayManager::cycleBrightness() {
    if (m_brightness == 255) {
        m_brightness = 64;  // 25% (Night mode)
        showToast("25%", COLOR_CYAN);
    } else if (m_brightness == 64) {
        m_brightness = 128; // 50%
        showToast("50%", COLOR_CYAN);
    } else if (m_brightness == 128) {
        m_brightness = 192; // 75%
        showToast("75%", COLOR_CYAN);
    } else {
        m_brightness = 255; // 100% (Day mode)
        showToast("100%", COLOR_SAFE);
    }
    analogWrite(PIN_LCD_BL, m_brightness);
}

void DisplayManager::toggleRotation() {
    m_rotation = (m_rotation == 0) ? 2 : 0;
    m_tft->setRotation(m_rotation);
    if (m_rotation == 0) {
        showToast("XOAY: 0*", COLOR_SAFE);
    } else {
        showToast("XOAY: 180*", COLOR_WARN);
    }
}

void DisplayManager::showToast(const char* text, uint16_t color) {
    strncpy(m_toastText, text, sizeof(m_toastText) - 1);
    m_toastText[sizeof(m_toastText) - 1] = '\0';
    m_toastColor = color;
    m_toastExpiry = millis() + 1800;
}

void DisplayManager::setRoadName(const char* name) {
    if (name && name[0] != '\0') {
        if (strcmp(m_roadName, name) != 0) {
            strncpy(m_roadName, name, sizeof(m_roadName) - 1);
            m_roadName[sizeof(m_roadName) - 1] = '\0';
            m_marqueeStartTime = millis(); // Reset scroll cycle on new road name
        }
    }
}

void DisplayManager::updateData(
    int currentSpeed,
    int speedLimit,
    int satellites,
    bool gpsFix,
    bool isDemoMode,
    bool isMuted,
    const char* roadName,
    bool wifiEnabled,
    bool wifiConnected,
    const char* wifiIp,
    bool isOtaUpdating,
    int otaProgress,
    bool alertActive,
    uint8_t alertType,
    uint16_t alertDistance
) {
    m_speed = currentSpeed;
    m_speedLimit = speedLimit;
    m_sats = satellites;
    m_fix = gpsFix;
    m_demo = isDemoMode;
    m_muted = isMuted;

    m_wifiEnabled = wifiEnabled;
    m_wifiConnected = wifiConnected;

    m_alertActive = alertActive;
    m_alertType = alertType;
    m_alertDistance = alertDistance;

    if (wifiIp) {
        strncpy(m_wifiIp, wifiIp, sizeof(m_wifiIp) - 1);
        m_wifiIp[sizeof(m_wifiIp) - 1] = '\0';
    }

    if (roadName && roadName[0] != '\0') {
        setRoadName(roadName);
    }
}

void DisplayManager::updateWiFiState(
    bool enabled,
    uint8_t workflowState,
    const char* workflowMsg,
    const char* ssid,
    const char* ipStr,
    int rssi,
    int progress,
    int countdownSec
) {
    m_wifiEnabled = enabled;
    m_otaWorkflowState = workflowState;
    m_wifiRssi = rssi;
    m_otaProgress = progress;
    m_otaCountdownSec = countdownSec;

    if (workflowMsg) {
        strncpy(m_otaWorkflowMsg, workflowMsg, sizeof(m_otaWorkflowMsg) - 1);
        m_otaWorkflowMsg[sizeof(m_otaWorkflowMsg) - 1] = '\0';
    } else {
        m_otaWorkflowMsg[0] = '\0';
    }

    if (ssid) {
        strncpy(m_wifiSsid, ssid, sizeof(m_wifiSsid) - 1);
        m_wifiSsid[sizeof(m_wifiSsid) - 1] = '\0';
    } else {
        m_wifiSsid[0] = '\0';
    }

    if (ipStr) {
        strncpy(m_wifiIp, ipStr, sizeof(m_wifiIp) - 1);
        m_wifiIp[sizeof(m_wifiIp) - 1] = '\0';
    } else {
        m_wifiIp[0] = '\0';
    }
}

void DisplayManager::drawTopBar() {
    // 1. Clear top bar area
    m_canvas->fillRect(0, 0, LCD_WIDTH, 22, COLOR_BG);

    // 2. Left Zone: GPS status / Demo Indicator
    if (m_demo) {
        m_canvas->fillCircle(8, 10, 3, COLOR_WARN);
        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(COLOR_WARN);
        m_canvas->setCursor(15, 7);
        m_canvas->print("D");
    } else {
        uint16_t gpsDotColor = m_fix ? COLOR_SAFE : COLOR_ALERT;
        m_canvas->fillCircle(8, 10, 3, gpsDotColor);
        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(COLOR_SILVER);
        m_canvas->setCursor(15, 7);
        m_canvas->printf("%d", m_sats);
    }

    // 3. Right Zone: WiFi & Mute Indicators
    if (m_wifiEnabled) {
        if (m_wifiConnected) {
            m_canvas->fillCircle(228, 10, 3, COLOR_SAFE);
            m_canvas->drawCircle(228, 10, 6, COLOR_SAFE);
        } else {
            bool blink = (millis() / 400) % 2;
            m_canvas->fillCircle(228, 10, 3, blink ? COLOR_CYAN : COLOR_TRACK);
        }
    } else if (m_muted) {
        m_canvas->fillRoundRect(218, 4, 18, 13, 2, COLOR_ALERT);
        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(COLOR_WHITE);
        m_canvas->setCursor(224, 7);
        m_canvas->print("X");
    }

    // 4. Center Area: Road Name
    const char* displayName = (m_roadName[0] != '\0') ? m_roadName : "VIETHUD HANOI";
    int nameLen = strlen(displayName);
    const int MAX_VIEW_CHARS = 28;

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_ROAD_TEXT);

    if (nameLen <= MAX_VIEW_CHARS) {
        int textW = nameLen * 6;
        int startX = 28 + (186 - textW) / 2;
        m_canvas->setCursor(startX, 7);
        m_canvas->print(displayName);
    } else {
        int maxOffset = nameLen - MAX_VIEW_CHARS;
        uint32_t now = millis();
        uint32_t pauseStartMs = 1500;
        uint32_t stepMs = 280;
        uint32_t scrollDurationMs = maxOffset * stepMs;
        uint32_t pauseEndMs = 1200;
        uint32_t totalCycleMs = pauseStartMs + scrollDurationMs + pauseEndMs;

        uint32_t elapsed = (now - m_marqueeStartTime) % totalCycleMs;
        int charOffset = 0;
        if (elapsed < pauseStartMs) {
            charOffset = 0;
        } else if (elapsed < pauseStartMs + scrollDurationMs) {
            charOffset = (elapsed - pauseStartMs) / stepMs;
            if (charOffset > maxOffset) charOffset = maxOffset;
        } else {
            charOffset = maxOffset;
        }

        char viewBuf[MAX_VIEW_CHARS + 2];
        strncpy(viewBuf, displayName + charOffset, MAX_VIEW_CHARS);
        viewBuf[MAX_VIEW_CHARS] = '\0';

        m_canvas->setCursor(31, 7);
        m_canvas->print(viewBuf);
    }

    // Divider Line separating Top Bar from Alert Area
    m_canvas->drawFastHLine(8, 22, 224, 0x18E3);
}

// ---------------------------------------------------------------------------
// TRAFFIC ALERT ICON DRAWING PRIMITIVES
// ---------------------------------------------------------------------------

void DisplayManager::drawIconCamera(int x, int y) {
    m_canvas->fillRoundRect(x, y, 42, 38, 4, 0x027B); // Highway Blue
    m_canvas->drawRoundRect(x, y, 42, 38, 4, COLOR_WHITE);

    m_canvas->fillRoundRect(x + 5, y + 10, 22, 16, 2, COLOR_WHITE);
    m_canvas->fillTriangle(x + 27, y + 13, x + 35, y + 9, x + 35, y + 27, COLOR_WHITE);
    m_canvas->fillTriangle(x + 27, y + 13, x + 35, y + 27, x + 27, y + 23, COLOR_WHITE);

    m_canvas->fillCircle(x + 13, y + 18, 4, COLOR_CYAN);
    m_canvas->drawCircle(x + 13, y + 18, 4, 0x0010);
    m_canvas->fillCircle(x + 12, y + 17, 1, COLOR_WHITE);

    m_canvas->fillRect(x + 10, y + 26, 4, 7, COLOR_WHITE);
    m_canvas->fillRect(x + 6, y + 31, 12, 3, COLOR_WHITE);

    bool blink = (millis() / 350) % 2;
    if (blink) {
        m_canvas->fillCircle(x + 23, y + 13, 2, COLOR_ALERT);
    }
}

void DisplayManager::drawIconTrafficLight(int x, int y) {
    m_canvas->fillRoundRect(x, y, 26, 40, 5, 0x10A2);
    m_canvas->drawRoundRect(x, y, 26, 40, 5, COLOR_SILVER);

    m_canvas->fillCircle(x + 13, y + 8, 5, COLOR_ALERT);
    m_canvas->drawCircle(x + 13, y + 8, 6, 0xC800);
    m_canvas->fillCircle(x + 11, y + 6, 1, COLOR_WHITE);

    m_canvas->fillCircle(x + 13, y + 20, 4, 0x4200);
    m_canvas->fillCircle(x + 13, y + 31, 4, 0x01E0);
    m_canvas->drawFastHLine(x + 7, y + 2, 12, COLOR_WHITE);
}

void DisplayManager::drawIconResidentArea(int x, int y) {
    m_canvas->fillRoundRect(x, y, 42, 38, 4, 0x027B);
    m_canvas->drawRoundRect(x, y, 42, 38, 4, COLOR_WHITE);

    m_canvas->fillRect(x + 6, y + 10, 14, 22, COLOR_WHITE);
    m_canvas->fillRect(x + 8, y + 13, 3, 4, 0x027B);
    m_canvas->fillRect(x + 14, y + 13, 3, 4, 0x027B);
    m_canvas->fillRect(x + 8, y + 20, 3, 4, 0x027B);
    m_canvas->fillRect(x + 14, y + 20, 3, 4, 0x027B);

    m_canvas->fillTriangle(x + 22, y + 18, x + 30, y + 11, x + 38, y + 18, COLOR_WHITE);
    m_canvas->fillRect(x + 23, y + 18, 14, 14, COLOR_WHITE);
    m_canvas->fillRect(x + 28, y + 24, 4, 8, 0x027B);
}

void DisplayManager::drawIconNoOvertaking(int x, int y) {
    int cx = x + 21;
    int cy = y + 19;
    int r = 18;

    for (int i = 0; i < 4; i++) {
        m_canvas->drawCircle(cx, cy, r - i, COLOR_SIGN_RED);
    }
    m_canvas->fillCircle(cx, cy, r - 4, COLOR_WHITE);

    m_canvas->fillRoundRect(cx + 2, cy - 5, 8, 10, 2, COLOR_BLACK);
    m_canvas->fillRect(cx + 4, cy - 2, 4, 4, 0x7BEF);

    m_canvas->fillRoundRect(cx - 10, cy - 5, 8, 10, 2, COLOR_ALERT);
    m_canvas->fillRect(cx - 8, cy - 2, 4, 4, COLOR_WHITE);
}

void DisplayManager::drawIconTollBooth(int x, int y) {
    m_canvas->fillRoundRect(x, y, 42, 38, 4, 0xFDE0);
    m_canvas->drawRoundRect(x, y, 42, 38, 4, COLOR_SIGN_RED);
    m_canvas->drawRoundRect(x + 1, y + 1, 40, 36, 3, COLOR_SIGN_RED);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_BLACK);
    m_canvas->setCursor(x + 4, y + 12);
    m_canvas->print("BOT");

    for (int i = 0; i < 32; i += 8) {
        m_canvas->fillRect(x + 5 + i, y + 29, 4, 3, COLOR_ALERT);
        m_canvas->fillRect(x + 9 + i, y + 29, 4, 3, COLOR_WHITE);
    }
}

void DisplayManager::drawIconDanger(int x, int y) {
    int x1 = x + 21, y1 = y + 2;
    int x2 = x + 2,  y2 = y + 36;
    int x3 = x + 40, y3 = y + 36;

    m_canvas->fillTriangle(x1, y1 + 3, x2 + 3, y2 - 2, x3 - 3, y3 - 2, 0xFFE0);

    for (int i = 0; i < 3; i++) {
        m_canvas->drawTriangle(x1, y1 + i, x2 + i, y2 - i, x3 - i, y3 - i, COLOR_SIGN_RED);
    }

    m_canvas->fillRect(x + 19, y + 12, 4, 11, COLOR_BLACK);
    m_canvas->fillRect(x + 19, y + 26, 4, 4, COLOR_BLACK);
}

// ---------------------------------------------------------------------------
// TRAFFIC ALERT AREA (Card y = 25..75)
// ---------------------------------------------------------------------------
void DisplayManager::drawTrafficAlertArea() {
    int boxX = 8;
    int boxY = 25;
    int boxW = 224;
    int boxH = 50;

    if (!m_alertActive || m_alertType == 0) {
        m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_CARD_BG);
        m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_CARD_BORDER);

        m_canvas->drawCircle(26, 49, 12, COLOR_TRACK);
        m_canvas->drawCircle(26, 49, 6, COLOR_SAFE);
        m_canvas->fillCircle(26, 49, 2, COLOR_SAFE);

        int sweepAngle = (millis() / 8) % 360;
        float rad = sweepAngle * 0.0174533f;
        int sx = 26 + (int)(cos(rad) * 11);
        int sy = 49 + (int)(sin(rad) * 11);
        m_canvas->drawLine(26, 49, sx, sy, COLOR_SAFE);

        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(COLOR_SILVER);
        m_canvas->setCursor(48, 38);
        m_canvas->print("VIETHUD HA NOI (200KM)");

        m_canvas->setTextColor(COLOR_SAFE);
        m_canvas->setCursor(48, 52);
        m_canvas->print("40,386 DIEM CANH BAO");
        return;
    }

    uint16_t alertColor = COLOR_WHITE;
    const char* alertTitle = "";

    switch (m_alertType) {
        case 1:
            alertColor = COLOR_ALERT;
            alertTitle = "CAM BAN TOC DO";
            break;
        case 4:
            alertColor = COLOR_ALERT;
            alertTitle = "CAMERA PHAT NGUOI";
            break;
        case 6:
            alertColor = COLOR_WARN;
            alertTitle = "CAM VUOT DEN DO";
            break;
        case 2:
            alertColor = COLOR_CYAN;
            alertTitle = "KHU DONG DAN CU";
            break;
        case 3:
            alertColor = COLOR_ALERT;
            alertTitle = "DOAN DUONG CAM VUOT";
            break;
        case 5:
            alertColor = COLOR_WARN;
            alertTitle = "TRAM THU PHI BOT";
            break;
        case 10:
        default:
            alertColor = COLOR_WARN;
            alertTitle = "CANH BAO NGUY HIEM";
            break;
    }

    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, alertColor);
    m_canvas->drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 5, alertColor);

    switch (m_alertType) {
        case 1:
        case 4:
            drawIconCamera(14, 31);
            break;
        case 6:
            drawIconTrafficLight(18, 30);
            break;
        case 2:
            drawIconResidentArea(14, 31);
            break;
        case 3:
            drawIconNoOvertaking(14, 31);
            break;
        case 5:
            drawIconTollBooth(14, 31);
            break;
        case 10:
        default:
            drawIconDanger(14, 31);
            break;
    }

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(alertColor);
    m_canvas->setCursor(64, 32);
    m_canvas->print(alertTitle);

    char distStr[12];
    if (m_alertDistance > 0) {
        snprintf(distStr, sizeof(distStr), "%d", m_alertDistance);
    } else {
        snprintf(distStr, sizeof(distStr), "--");
    }

    m_canvas->setFont(&FreeSansBold18pt7b);
    m_canvas->setTextColor(COLOR_WHITE);

    int16_t x1, y1;
    uint16_t w, h;
    m_canvas->getTextBounds(distStr, 0, 0, &x1, &y1, &w, &h);

    m_canvas->setCursor(64, 57);
    m_canvas->print(distStr);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_SILVER);
    m_canvas->setCursor(64 + w + 4, 48);
    m_canvas->print("m");

    int barX = 64;
    int barY = 64;
    int barW = 158;
    int barH = 3;

    m_canvas->fillRect(barX, barY, barW, barH, COLOR_TRACK);

    float maxDist = 500.0f;
    float distRatio = (float)m_alertDistance / maxDist;
    if (distRatio > 1.0f) distRatio = 1.0f;
    if (distRatio < 0.0f) distRatio = 0.0f;

    int fillW = (int)(distRatio * barW);
    if (fillW > 0) {
        m_canvas->fillRect(barX, barY, fillW, barH, alertColor);
    }
}

// ---------------------------------------------------------------------------
// SPEED LIMIT SIGN (Dead Center: cx=120, cy=127, radius=46)
// ---------------------------------------------------------------------------
void DisplayManager::drawSpeedLimitSign(int cx, int cy, int radius, int limit) {
    bool isOver = (m_speedLimit > 0 && m_speed > m_speedLimit);

    uint32_t now = millis();
    if (now - m_lastFlashTime > 300) {
        m_lastFlashTime = now;
        m_flashState = !m_flashState;
    }

    uint16_t outerBorderColor = COLOR_SIGN_RED;
    uint16_t innerBgColor = COLOR_WHITE;
    uint16_t textColor = COLOR_BLACK;

    if (isOver && m_flashState) {
        outerBorderColor = COLOR_WHITE;
        innerBgColor = COLOR_ALERT;
        textColor = COLOR_WHITE;
    }

    m_canvas->drawCircle(cx, cy, radius + 1, 0x18C3);

    for (int r = radius; r >= radius - 8; r--) {
        m_canvas->drawCircle(cx, cy, r, outerBorderColor);
    }

    m_canvas->fillCircle(cx, cy, radius - 9, innerBgColor);

    char buf[12];
    if (limit <= 0) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%d", limit);
    }

    const GFXfont* fontToUse = (limit >= 100) ? &FreeSansBold18pt7b : &FreeSansBold24pt7b;
    m_canvas->setFont(fontToUse);
    m_canvas->setTextColor(textColor);

    int16_t x1, y1;
    uint16_t w, h;
    m_canvas->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);

    int cursorX = cx - (w / 2) - x1;
    int cursorY = cy - (h / 2) - y1;

    m_canvas->setCursor(cursorX, cursorY);
    m_canvas->print(buf);

    m_canvas->setFont(NULL);
}

// ---------------------------------------------------------------------------
// CURRENT SPEED SECTION (Bottom card: cx=120, cy=206, h=54)
// ---------------------------------------------------------------------------
void DisplayManager::drawCurrentSpeedSection(int cx, int cy, int speed, int limit) {
    bool isOver = (limit > 0 && speed > limit);
    bool isNearLimit = (!isOver && limit > 0 && speed >= limit - 5);
    int delta = speed - limit;

    int cardW = 216;
    int cardH = 54;
    int cardX = cx - cardW / 2;
    int cardY = cy - cardH / 2;

    m_canvas->fillRoundRect(cardX, cardY, cardW, cardH, 8, COLOR_CARD_BG);

    uint16_t statusColor;
    if (isOver) {
        statusColor = COLOR_ALERT;
    } else if (isNearLimit) {
        statusColor = COLOR_WARN;
    } else {
        statusColor = COLOR_SAFE;
    }

    m_canvas->drawRoundRect(cardX, cardY, cardW, cardH, 8, statusColor);
    if (isOver) {
        m_canvas->drawRoundRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, 7, COLOR_ALERT);
    }

    char speedStr[8];
    snprintf(speedStr, sizeof(speedStr), "%d", speed);

    m_canvas->setFont(&FreeSansBold24pt7b);
    m_canvas->setTextColor(statusColor);

    int16_t x1, y1;
    uint16_t w, h;
    m_canvas->getTextBounds(speedStr, 0, 0, &x1, &y1, &w, &h);

    int speedX = cardX + 22;
    int speedY = cardY + 36;
    m_canvas->setCursor(speedX, speedY);
    m_canvas->print(speedStr);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(isOver ? COLOR_ALERT : COLOR_CYAN);
    int unitX = speedX + w + 10;
    m_canvas->setCursor(unitX, cardY + 14);
    m_canvas->print("km/h");

    if (isOver && delta > 0) {
        char deltaStr[8];
        snprintf(deltaStr, sizeof(deltaStr), "+%d", delta);
        int tagW = 46;
        int tagH = 24;
        int tagX = cardX + cardW - tagW - 12;
        int tagY = cardY + 10;

        m_canvas->fillRoundRect(tagX, tagY, tagW, tagH, 5, COLOR_ALERT);
        m_canvas->setTextSize(2);
        m_canvas->setTextColor(COLOR_WHITE);
        m_canvas->setCursor(tagX + 6, tagY + 4);
        m_canvas->print(deltaStr);
    }

    int barX = cardX + 12;
    int barY = cardY + cardH - 7;
    int barW = cardW - 24;
    int barH = 3;

    m_canvas->fillRect(barX, barY, barW, barH, COLOR_TRACK);

    float ratio = 0.0f;
    if (limit > 0) {
        ratio = (float)speed / (float)limit;
        if (ratio > 1.25f) ratio = 1.25f;
    }
    int fillW = (int)round(ratio * (barW * 0.8f));
    if (fillW > barW) fillW = barW;
    if (fillW > 0) {
        m_canvas->fillRect(barX, barY, fillW, barH, statusColor);
    }
    int tickX = barX + (int)round(barW * 0.8f);
    m_canvas->drawFastVLine(tickX, barY - 2, barH + 4, COLOR_WHITE);
}

void DisplayManager::drawToast() {
    if (millis() < m_toastExpiry && m_toastText[0] != '\0') {
        int textLen = strlen(m_toastText);
        int boxW = textLen * 9 + 24;
        int boxH = 26;
        int boxX = 120 - boxW / 2;
        int boxY = 82;

        m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_TOAST_BG);
        m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, m_toastColor);

        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(m_toastColor);
        m_canvas->setCursor(boxX + 12, boxY + 9);
        m_canvas->print(m_toastText);
    }
}

// ---------------------------------------------------------------------------
// DEDICATED FULL-SCREEN WIFI & OTA PAGE (Trang Kết Nối & Cập Nhật)
// ---------------------------------------------------------------------------
void DisplayManager::drawWiFiOTAPage() {
    // 1. Header Bar (y: 0 .. 30)
    m_canvas->fillRect(0, 0, LCD_WIDTH, 30, COLOR_BG);
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_CYAN);
    m_canvas->setCursor(54, 8);
    m_canvas->print("WIFI & OTA");

    m_canvas->drawFastHLine(8, 30, 224, COLOR_CARD_BORDER);

    // 2. Determine Theme Color based on OTAWorkflowState:
    // 1: CONNECTING_WIFI, 2: WIFI_CONNECTED, 3: CHECKING_UPDATE,
    // 4: DOWNLOADING, 5: UP_TO_DATE, 6: FAILED, 7: REBOOTING
    uint16_t cardBorderColor = COLOR_CYAN;
    if (m_otaWorkflowState == 5) {
        cardBorderColor = COLOR_SAFE; // Green
    } else if (m_otaWorkflowState == 4 || m_otaWorkflowState == 7) {
        cardBorderColor = COLOR_WARN; // Amber
    } else if (m_otaWorkflowState == 6) {
        cardBorderColor = COLOR_ALERT; // Red
    }

    int boxX = 8, boxY = 36, boxW = 224, boxH = 158;
    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 8, cardBorderColor);
    m_canvas->drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 7, cardBorderColor);

    int cx = 120;

    // 3. Central Graphic & Animation (y: 44 .. 92)
    if (m_otaWorkflowState == 1) { // CONNECTING_WIFI
        int wavePhase = (millis() / 240) % 4;
        m_canvas->fillCircle(cx, 84, 3, COLOR_CYAN);

        if (wavePhase >= 1) {
            m_canvas->drawCircle(cx, 84, 10, COLOR_CYAN);
            m_canvas->drawCircle(cx, 84, 11, COLOR_CYAN);
            m_canvas->fillRect(cx - 15, 85, 30, 15, COLOR_CARD_BG);
        }
        if (wavePhase >= 2) {
            m_canvas->drawCircle(cx, 84, 18, COLOR_CYAN);
            m_canvas->drawCircle(cx, 84, 19, COLOR_CYAN);
            m_canvas->fillRect(cx - 24, 85, 48, 20, COLOR_CARD_BG);
        }
        if (wavePhase >= 3) {
            m_canvas->drawCircle(cx, 84, 26, COLOR_CYAN);
            m_canvas->drawCircle(cx, 84, 27, COLOR_CYAN);
            m_canvas->fillRect(cx - 32, 85, 64, 25, COLOR_CARD_BG);
        }
    } else if (m_otaWorkflowState == 2 || m_otaWorkflowState == 3) { // CONNECTED / CHECKING
        m_canvas->fillCircle(cx, 68, 18, 0x01E0);
        m_canvas->drawCircle(cx, 68, 18, COLOR_SAFE);
        m_canvas->drawCircle(cx, 68, 10, COLOR_SAFE);

        int angle = (millis() / 6) % 360;
        float rad = angle * 0.0174533f;
        int sx = cx + (int)(cos(rad) * 14);
        int sy = 68 + (int)(sin(rad) * 14);
        m_canvas->drawLine(cx, 68, sx, sy, COLOR_WHITE);
        m_canvas->fillCircle(sx, sy, 2, COLOR_WHITE);
    } else if (m_otaWorkflowState == 4 || m_otaWorkflowState == 7) { // DOWNLOADING / REBOOTING
        m_canvas->fillCircle(cx, 68, 18, 0x5280);
        m_canvas->drawCircle(cx, 68, 18, COLOR_WARN);

        m_canvas->fillRect(cx - 2, 54, 5, 14, COLOR_WHITE);
        m_canvas->fillTriangle(cx - 9, 68, cx + 9, 68, cx, 80, COLOR_WHITE);
    } else if (m_otaWorkflowState == 5) { // UP_TO_DATE
        m_canvas->fillCircle(cx, 68, 18, COLOR_SAFE);
        m_canvas->drawLine(cx - 9, 68, cx - 3, 76, COLOR_WHITE);
        m_canvas->drawLine(cx - 8, 68, cx - 2, 76, COLOR_WHITE);
        m_canvas->drawLine(cx - 3, 76, cx + 9, 58, COLOR_WHITE);
        m_canvas->drawLine(cx - 2, 76, cx + 10, 58, COLOR_WHITE);
    } else { // FAILED (6)
        m_canvas->fillTriangle(cx, 50, cx - 18, 84, cx + 18, 84, COLOR_ALERT);
        m_canvas->fillTriangle(cx, 53, cx - 15, 82, cx + 15, 82, COLOR_WHITE);
        m_canvas->fillRect(cx - 1, 60, 3, 12, COLOR_BLACK);
        m_canvas->fillRect(cx - 1, 75, 3, 3, COLOR_BLACK);
    }

    // 4. Main Stage Title Text (y: 98)
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(cardBorderColor);

    const char* titleMsg = m_otaWorkflowMsg[0] != '\0' ? m_otaWorkflowMsg : "DANG XU LY...";
    int textW = strlen(titleMsg) * 6;
    int textX = cx - textW / 2;
    if (textX < boxX + 6) textX = boxX + 6;
    m_canvas->setCursor(textX, 98);
    m_canvas->print(titleMsg);

    // 5. Detailed Body Info per state
    if (m_otaWorkflowState == 1) { // CONNECTING
        m_canvas->setTextColor(COLOR_SILVER);
        m_canvas->setCursor(boxX + 16, 122);
        m_canvas->print("1. iPhone cua Pham");
        m_canvas->setCursor(boxX + 16, 138);
        m_canvas->print("2. VuPQ");
        m_canvas->setTextColor(COLOR_CYAN);
        m_canvas->setCursor(boxX + 16, 158);
        m_canvas->print("Dang do tim tin hieu...");
    } else if (m_otaWorkflowState == 2 || m_otaWorkflowState == 3) { // CONNECTED / CHECKING
        m_canvas->setTextColor(COLOR_WHITE);
        m_canvas->setCursor(boxX + 16, 120);
        m_canvas->printf("IP: %s", m_wifiIp);

        m_canvas->setTextColor(COLOR_SAFE);
        m_canvas->setCursor(boxX + 16, 136);
        m_canvas->printf("Song: %d dBm (Tot)", m_wifiRssi);

        m_canvas->setTextColor(COLOR_CYAN);
        m_canvas->setCursor(boxX + 16, 156);
        m_canvas->print("Pi4: http://192.168.1.65/");
    } else if (m_otaWorkflowState == 4 || m_otaWorkflowState == 7) { // DOWNLOADING
        int barX = boxX + 16;
        int barY = 120;
        int barW = boxW - 32;
        int barH = 12;

        m_canvas->fillRect(barX, barY, barW, barH, COLOR_TRACK);
        m_canvas->drawRect(barX, barY, barW, barH, COLOR_CARD_BORDER);

        int fillW = (m_otaProgress * barW) / 100;
        if (fillW > 0) {
            m_canvas->fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, COLOR_SAFE);
        }

        char pctBuf[12];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", m_otaProgress);
        m_canvas->setFont(&FreeSansBold18pt7b);
        m_canvas->setTextColor(COLOR_WHITE);

        int16_t x1, y1;
        uint16_t w, h;
        m_canvas->getTextBounds(pctBuf, 0, 0, &x1, &y1, &w, &h);
        m_canvas->setCursor(cx - (w / 2) - x1, 158);
        m_canvas->print(pctBuf);

        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(COLOR_ALERT);
        m_canvas->setCursor(boxX + 32, 172);
        m_canvas->print("KHONG NGAT NGUON !");
    } else if (m_otaWorkflowState == 5) { // UP_TO_DATE
        m_canvas->setTextColor(COLOR_SAFE);
        m_canvas->setCursor(boxX + 16, 120);
        m_canvas->print("Phien ban: v3.3.0");

        m_canvas->setTextColor(COLOR_CYAN);
        m_canvas->setCursor(boxX + 16, 136);
        m_canvas->printf("Web OTA: http://%s/", m_wifiIp);

        m_canvas->setTextColor(COLOR_WARN);
        m_canvas->setCursor(boxX + 16, 158);
        m_canvas->printf("Tu dong ve HUD sau: %ds", m_otaCountdownSec);
    } else { // FAILED
        m_canvas->setTextColor(COLOR_SILVER);
        m_canvas->setCursor(boxX + 16, 120);
        m_canvas->print("Khong tim thay AP hoac Server");
        m_canvas->setCursor(boxX + 16, 136);
        m_canvas->print("Kiem tra lai Pi 4 / Hotspot");

        m_canvas->setTextColor(COLOR_WARN);
        m_canvas->setCursor(boxX + 16, 158);
        m_canvas->printf("Tu dong ve HUD sau: %ds", m_otaCountdownSec);
    }

    // 6. Footer Button Guide (y: 202 .. 232)
    m_canvas->fillRoundRect(16, 202, 208, 28, 6, COLOR_CARD_BG);
    m_canvas->drawRoundRect(16, 202, 208, 28, 6, COLOR_CARD_BORDER);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_WHITE);
    m_canvas->setCursor(32, 211);
    m_canvas->print("[ NUT GIUA: VE TRANG HUD ]");
}

void DisplayManager::render() {
    if (!m_canvas) return;

    // Clear frame to deep black
    m_canvas->fillScreen(COLOR_BG);

    // If WiFi is enabled, switch completely to the dedicated WiFi & OTA connection screen!
    if (m_wifiEnabled) {
        drawWiFiOTAPage();
        m_canvas->flush();
        return;
    }

    // Normal HUD Dashboard Mode
    // 1. Draw Top Bar with Road Name & GPS sats
    drawTopBar();

    // 2. Draw Live Traffic Alert Area (Visual Icons / Signs & Distance countdown)
    drawTrafficAlertArea();

    // 3. Draw MAIN HIGHLIGHT: Huge Speed Limit Sign (Dead Center: cx=120, cy=127, radius=46)
    drawSpeedLimitSign(120, 127, 46, m_speedLimit);

    // 4. Draw Cockpit Vehicle Speed Section (cx=120, cy=206, h=54)
    drawCurrentSpeedSection(120, 206, m_speed, m_speedLimit);

    // 5. Draw Toast Popup if active
    drawToast();

    // Flush entire frame buffer to ST7789 via high-speed SPI
    m_canvas->flush();
}
