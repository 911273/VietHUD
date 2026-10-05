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
      m_wifiEnabled(false), m_wifiConnected(false), m_isOtaUpdating(false), m_otaProgress(0),
      m_alertActive(false), m_alertType(0), m_alertDistance(0),
      m_brightness(255), m_rotation(0),
      m_lastFlashTime(0), m_flashState(false),
      m_toastColor(COLOR_WHITE), m_toastExpiry(0) {
    m_toastText[0] = '\0';
    m_wifiIp[0] = '\0';
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
    m_isOtaUpdating = isOtaUpdating;
    m_otaProgress = otaProgress;

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
    // Available width: x = 28 to 214 (186 pixels).
    // At setTextSize(1) (6px per char), fits up to 28 characters without wrapping.
    const char* displayName = (m_roadName[0] != '\0') ? m_roadName : "VIETHUD HANOI";
    int nameLen = strlen(displayName);
    const int MAX_VIEW_CHARS = 28;

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_ROAD_TEXT);

    if (nameLen <= MAX_VIEW_CHARS) {
        // Fits completely: perfectly centered
        int textW = nameLen * 6;
        int startX = 28 + (186 - textW) / 2;
        m_canvas->setCursor(startX, 7);
        m_canvas->print(displayName);
    } else {
        // Long road name: smooth step ticker by character slicing (guarantees zero coordinate wrapping)
        int maxOffset = nameLen - MAX_VIEW_CHARS;
        uint32_t now = millis();
        uint32_t pauseStartMs = 1500;
        uint32_t stepMs = 280; // 280ms per character
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
// TRAFFIC ALERT ICON DRAWING PRIMITIVES (Biểu tượng / Biển báo trực quan)
// ---------------------------------------------------------------------------

// 1. Camera icon: Blue rectangular sign with authentic CCTV silhouette & cyan lens
void DisplayManager::drawIconCamera(int x, int y) {
    m_canvas->fillRoundRect(x, y, 42, 38, 4, 0x027B); // Highway Blue
    m_canvas->drawRoundRect(x, y, 42, 38, 4, COLOR_WHITE);

    // Camera body (white silhouette)
    m_canvas->fillRoundRect(x + 5, y + 10, 22, 16, 2, COLOR_WHITE);
    // Lens cone pointing right
    m_canvas->fillTriangle(x + 27, y + 13, x + 35, y + 9, x + 35, y + 27, COLOR_WHITE);
    m_canvas->fillTriangle(x + 27, y + 13, x + 35, y + 27, x + 27, y + 23, COLOR_WHITE);

    // Cyan lens reflection
    m_canvas->fillCircle(x + 13, y + 18, 4, COLOR_CYAN);
    m_canvas->drawCircle(x + 13, y + 18, 4, 0x0010);
    m_canvas->fillCircle(x + 12, y + 17, 1, COLOR_WHITE);

    // Mounting stem at bottom
    m_canvas->fillRect(x + 10, y + 26, 4, 7, COLOR_WHITE);
    m_canvas->fillRect(x + 6, y + 31, 12, 3, COLOR_WHITE);

    // Blinking red recording LED
    bool blink = (millis() / 350) % 2;
    if (blink) {
        m_canvas->fillCircle(x + 23, y + 13, 2, COLOR_ALERT);
    }
}

// 2. Traffic Light: 3-lamp housing with bright glowing red lamp
void DisplayManager::drawIconTrafficLight(int x, int y) {
    m_canvas->fillRoundRect(x, y, 26, 40, 5, 0x10A2); // Dark slate/charcoal
    m_canvas->drawRoundRect(x, y, 26, 40, 5, COLOR_SILVER);

    // Red Lamp (TOP) - Active glowing red with lens reflection
    m_canvas->fillCircle(x + 13, y + 8, 5, COLOR_ALERT);
    m_canvas->drawCircle(x + 13, y + 8, 6, 0xC800);
    m_canvas->fillCircle(x + 11, y + 6, 1, COLOR_WHITE);

    // Yellow Lamp (MIDDLE) - Inactive dark amber
    m_canvas->fillCircle(x + 13, y + 20, 4, 0x4200);

    // Green Lamp (BOTTOM) - Inactive dark green
    m_canvas->fillCircle(x + 13, y + 31, 4, 0x01E0);

    // Visor at top
    m_canvas->drawFastHLine(x + 7, y + 2, 12, COLOR_WHITE);
}

// 3. Populated Area (Biển R.420): Blue rectangle with white buildings
void DisplayManager::drawIconResidentArea(int x, int y) {
    m_canvas->fillRoundRect(x, y, 42, 38, 4, 0x027B); // Official Vietnam Highway Blue
    m_canvas->drawRoundRect(x, y, 42, 38, 4, COLOR_WHITE);

    // Left building (High-rise):
    m_canvas->fillRect(x + 6, y + 10, 14, 22, COLOR_WHITE);
    // Blue windows on left building
    m_canvas->fillRect(x + 8, y + 13, 3, 4, 0x027B);
    m_canvas->fillRect(x + 14, y + 13, 3, 4, 0x027B);
    m_canvas->fillRect(x + 8, y + 20, 3, 4, 0x027B);
    m_canvas->fillRect(x + 14, y + 20, 3, 4, 0x027B);

    // Right house (Gable roof house):
    m_canvas->fillTriangle(x + 22, y + 18, x + 30, y + 11, x + 38, y + 18, COLOR_WHITE);
    m_canvas->fillRect(x + 23, y + 18, 14, 14, COLOR_WHITE);
    m_canvas->fillRect(x + 28, y + 24, 4, 8, 0x027B); // Doorway
}

// 4. No Overtaking (Biển P.125): Red circle, white inner, red and black cars
void DisplayManager::drawIconNoOvertaking(int x, int y) {
    int cx = x + 21;
    int cy = y + 19;
    int r = 18;

    // Bold red circular border (4px thick)
    for (int i = 0; i < 4; i++) {
        m_canvas->drawCircle(cx, cy, r - i, COLOR_SIGN_RED);
    }
    m_canvas->fillCircle(cx, cy, r - 4, COLOR_WHITE);

    // Right car (Black silhouette):
    m_canvas->fillRoundRect(cx + 2, cy - 5, 8, 10, 2, COLOR_BLACK);
    m_canvas->fillRect(cx + 4, cy - 2, 4, 4, 0x7BEF);

    // Left car (Red overtaking car):
    m_canvas->fillRoundRect(cx - 10, cy - 5, 8, 10, 2, COLOR_ALERT);
    m_canvas->fillRect(cx - 8, cy - 2, 4, 4, COLOR_WHITE);
}

// 5. Toll Booth (Biển BOT P.135): Yellow sign with bold "BOT" and barie
void DisplayManager::drawIconTollBooth(int x, int y) {
    m_canvas->fillRoundRect(x, y, 42, 38, 4, 0xFDE0); // Vivid Highway Yellow
    m_canvas->drawRoundRect(x, y, 42, 38, 4, COLOR_SIGN_RED);
    m_canvas->drawRoundRect(x + 1, y + 1, 40, 36, 3, COLOR_SIGN_RED);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_BLACK);
    m_canvas->setCursor(x + 4, y + 12);
    m_canvas->print("BOT");

    // Striped barrier bar at bottom
    for (int i = 0; i < 32; i += 8) {
        m_canvas->fillRect(x + 5 + i, y + 29, 4, 3, COLOR_ALERT);
        m_canvas->fillRect(x + 9 + i, y + 29, 4, 3, COLOR_WHITE);
    }
}

// 6. Danger Sign (Biển W.208 / W.233): Yellow triangle, red border, black "!"
void DisplayManager::drawIconDanger(int x, int y) {
    int x1 = x + 21, y1 = y + 2;
    int x2 = x + 2,  y2 = y + 36;
    int x3 = x + 40, y3 = y + 36;

    m_canvas->fillTriangle(x1, y1 + 3, x2 + 3, y2 - 2, x3 - 3, y3 - 2, 0xFFE0);

    for (int i = 0; i < 3; i++) {
        m_canvas->drawTriangle(x1, y1 + i, x2 + i, y2 - i, x3 - i, y3 - i, COLOR_SIGN_RED);
    }

    // Black exclamation mark
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
        // Quiet State: Clean minimal radar scanner
        m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_CARD_BG);
        m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_CARD_BORDER);

        // Miniature radar scan icon on left
        m_canvas->drawCircle(26, 49, 12, COLOR_TRACK);
        m_canvas->drawCircle(26, 49, 6, COLOR_SAFE);
        m_canvas->fillCircle(26, 49, 2, COLOR_SAFE);

        int sweepAngle = (millis() / 8) % 360;
        float rad = sweepAngle * 0.0174533f;
        int sx = 26 + (int)(cos(rad) * 11);
        int sy = 49 + (int)(sin(rad) * 11);
        m_canvas->drawLine(26, 49, sx, sy, COLOR_SAFE);

        // Reassuring status text
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

    // ACTIVE ALERT: Traffic Alert Card with Visual Icon & Countdown Distance
    uint16_t alertColor = COLOR_WHITE;
    const char* alertTitle = "";

    switch (m_alertType) {
        case 1: // ALERT_SPEED_CAMERA
            alertColor = COLOR_ALERT;
            alertTitle = "CAM BAN TOC DO";
            break;
        case 4: // ALERT_CAMERA
            alertColor = COLOR_ALERT;
            alertTitle = "CAMERA PHAT NGUOI";
            break;
        case 6: // ALERT_TRAFFIC_LIGHT
            alertColor = COLOR_WARN;
            alertTitle = "CAM VUOT DEN DO";
            break;
        case 2: // ALERT_RESIDENT_AREA
            alertColor = COLOR_CYAN;
            alertTitle = "KHU DONG DAN CU";
            break;
        case 3: // ALERT_NO_OVERTAKING
            alertColor = COLOR_ALERT;
            alertTitle = "DOAN DUONG CAM VUOT";
            break;
        case 5: // ALERT_TOLL_BOOTH
            alertColor = COLOR_WARN;
            alertTitle = "TRAM THU PHI BOT";
            break;
        case 10: // ALERT_DANGER
        default:
            alertColor = COLOR_WARN;
            alertTitle = "CANH BAO NGUY HIEM";
            break;
    }

    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, alertColor);
    m_canvas->drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 5, alertColor);

    // 1. Draw Visual Sign / Icon on Left: x = 14, y = 31
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

    // 2. Alert Title Text (Top Right)
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(alertColor);
    m_canvas->setCursor(64, 32);
    m_canvas->print(alertTitle);

    // 3. Countdown Distance in Large Digits (FreeSansBold18pt7b)
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

    // Unit "m" in small font
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_SILVER);
    m_canvas->setCursor(64 + w + 4, 48);
    m_canvas->print("m");

    // 4. Distance Countdown Progress Bar
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

    // Flash animation every 300ms when overspeeding
    uint32_t now = millis();
    if (now - m_lastFlashTime > 300) {
        m_lastFlashTime = now;
        m_flashState = !m_flashState;
    }

    uint16_t outerBorderColor = COLOR_SIGN_RED;
    uint16_t innerBgColor = COLOR_WHITE;
    uint16_t textColor = COLOR_BLACK;

    if (isOver && m_flashState) {
        // High-contrast inverted emergency flash
        outerBorderColor = COLOR_WHITE;
        innerBgColor = COLOR_ALERT;
        textColor = COLOR_WHITE;
    }

    // Outer subtle contrast halo
    m_canvas->drawCircle(cx, cy, radius + 1, 0x18C3);

    // Massive bold red border (8px thick)
    for (int r = radius; r >= radius - 8; r--) {
        m_canvas->drawCircle(cx, cy, r, outerBorderColor);
    }

    // Pure white center disk
    m_canvas->fillCircle(cx, cy, radius - 9, innerBgColor);

    // Format text
    char buf[12];
    if (limit <= 0) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%d", limit);
    }

    // Use FreeSansBold fonts: 24pt for 2 digits, 18pt for 3 digits (100, 120)
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
        statusColor = COLOR_ALERT; // Red
    } else if (isNearLimit) {
        statusColor = COLOR_WARN;  // Amber
    } else {
        statusColor = COLOR_SAFE;  // Emerald Green
    }

    m_canvas->drawRoundRect(cardX, cardY, cardW, cardH, 8, statusColor);
    if (isOver) {
        m_canvas->drawRoundRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, 7, COLOR_ALERT);
    }

    // Speed digits: FreeSansBold24pt7b
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

    // Unit "km/h"
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(isOver ? COLOR_ALERT : COLOR_CYAN);
    int unitX = speedX + w + 10;
    m_canvas->setCursor(unitX, cardY + 14);
    m_canvas->print("km/h");

    // Overspeed delta tag (+XX) if exceeding limit
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

    // Automotive speed progress bar at bottom of card
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
        int boxY = 82; // Positioned cleanly between alert card and speed sign

        m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_TOAST_BG);
        m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, m_toastColor);

        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(m_toastColor);
        m_canvas->setCursor(boxX + 12, boxY + 9);
        m_canvas->print(m_toastText);
    }
}

void DisplayManager::drawOTAOverlay() {
    m_canvas->fillRoundRect(16, 30, 208, 180, 10, COLOR_CARD_BG);
    m_canvas->drawRoundRect(16, 30, 208, 180, 10, COLOR_CYAN);
    m_canvas->drawRoundRect(17, 31, 206, 178, 9, COLOR_CYAN);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_CYAN);
    m_canvas->setCursor(44, 48);
    m_canvas->print("OTA UPDATE");

    int barX = 36;
    int barY = 96;
    int barW = 168;
    int barH = 14;

    m_canvas->fillRect(barX, barY, barW, barH, COLOR_TRACK);
    m_canvas->drawRect(barX, barY, barW, barH, COLOR_CARD_BORDER);

    int progressFill = (m_otaProgress * barW) / 100;
    if (progressFill > 0) {
        m_canvas->fillRect(barX + 1, barY + 1, progressFill - 2, barH - 2, COLOR_SAFE);
    }

    char pctStr[8];
    snprintf(pctStr, sizeof(pctStr), "%d%%", m_otaProgress);
    m_canvas->setFont(&FreeSansBold18pt7b);
    m_canvas->setTextColor(COLOR_WHITE);

    int16_t x1, y1;
    uint16_t w, h;
    m_canvas->getTextBounds(pctStr, 0, 0, &x1, &y1, &w, &h);
    m_canvas->setCursor(120 - (w / 2) - x1, 148);
    m_canvas->print(pctStr);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_SILVER);
    m_canvas->setCursor(40, 178);
    m_canvas->print("KHONG NGAT NGUON !");
}

void DisplayManager::render() {
    if (!m_canvas) return;

    // Clear frame to deep black
    m_canvas->fillScreen(COLOR_BG);

    if (m_isOtaUpdating) {
        drawOTAOverlay();
        m_canvas->flush();
        return;
    }

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
