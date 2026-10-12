#include "DisplayManager.h"
#include "WiFiOTAManager.h"
#include "SettingsManager.h"
#include "VnFonts.h"
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

#define TOP_BAR_H         27     // road-name bar height (separator line drawn at this y)

// strncpy for UTF-8: never cuts a multi-byte character in half.
static void copyUtf8(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

DisplayManager::DisplayManager()
    : m_bus(nullptr), m_tft(nullptr), m_canvas(nullptr),
      m_speed(0), m_speedLimit(60), m_sats(0), m_fix(false), m_muted(false),
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
    copyUtf8(m_roadName, sizeof(m_roadName), "VietHUD");
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

void DisplayManager::setBrightness(uint8_t pct) {
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    m_brightness = (uint8_t)((pct * 255) / 100);
    analogWrite(PIN_LCD_BL, m_brightness);
}

void DisplayManager::showToast(const char* text, uint16_t color) {
    copyUtf8(m_toastText, sizeof(m_toastText), text);
    m_toastColor = color;
    m_toastExpiry = millis() + 1800;
}

void DisplayManager::setBattery(bool present, uint8_t level, bool charging) {
    m_batPresent = present;
    m_batLevel = level > 100 ? 100 : level;
    m_batCharging = charging;
}

// Battery outline + fill + percentage, right-aligned at xRight. Lightning bolt while charging.
int DisplayManager::drawBatteryIcon(int xRight, int midY) {
    const int bodyW = 20, bodyH = 11;
    int bx = xRight - 2 - bodyW;       // 2 px for the terminal nub
    int by = midY - bodyH / 2;
    uint16_t col = m_batCharging ? COLOR_SAFE
                 : m_batLevel <= 15 ? COLOR_ALERT
                 : m_batLevel <= 35 ? COLOR_WARN : COLOR_SILVER;

    m_canvas->drawRoundRect(bx, by, bodyW, bodyH, 2, col);
    m_canvas->fillRect(bx + bodyW, midY - 2, 2, 5, col);
    int fillW = ((bodyW - 4) * m_batLevel + 50) / 100;
    if (fillW > 0) m_canvas->fillRect(bx + 2, by + 2, fillW, bodyH - 4, col);

    if (m_batCharging) {
        // bolt, outlined in black so it reads on top of the fill
        int cx = bx + bodyW / 2;
        int16_t xs[6] = {(int16_t)(cx + 2), (int16_t)(cx - 3), (int16_t)cx, (int16_t)(cx - 2), (int16_t)(cx + 3), (int16_t)cx};
        int16_t ys[6] = {(int16_t)(by - 2), (int16_t)(midY + 1), (int16_t)(midY + 1), (int16_t)(by + bodyH + 1),
                         (int16_t)(midY - 1), (int16_t)(midY - 1)};
        m_canvas->fillTriangle(xs[0] + 1, ys[0], xs[1] - 1, ys[1], xs[2], ys[2], COLOR_BLACK);
        m_canvas->fillTriangle(xs[3] - 1, ys[3], xs[4] + 1, ys[4], xs[5], ys[5], COLOR_BLACK);
        m_canvas->fillTriangle(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], COLOR_ROAD_TEXT);
        m_canvas->fillTriangle(xs[3], ys[3], xs[4], ys[4], xs[5], ys[5], COLOR_ROAD_TEXT);
    }

    char pct[6];
    snprintf(pct, sizeof(pct), "%d%%", m_batLevel);
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(col);
    int tx = bx - 3 - (int)strlen(pct) * 6;
    m_canvas->setCursor(tx, midY - 3);
    m_canvas->print(pct);
    return tx;
}

void DisplayManager::setRoadName(const char* name) {
    if (name && name[0] != '\0') {
        char buf[sizeof(m_roadName)];
        copyUtf8(buf, sizeof(buf), name);
        if (strcmp(m_roadName, buf) != 0) {
            memcpy(m_roadName, buf, sizeof(m_roadName));
            m_marqueeStartTime = millis();
        }
    }
}

void DisplayManager::updateData(
    int currentSpeed,
    int speedLimit,
    int satellites,
    bool gpsFix,
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
    m_canvas->fillRect(0, 0, LCD_WIDTH, TOP_BAR_H, COLOR_BG);
    const int midY = TOP_BAR_H / 2;

    // Left: GPS fix dot + satellite count
    m_canvas->fillCircle(8, midY, 3, m_fix ? COLOR_SAFE : COLOR_ALERT);
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_SILVER);
    m_canvas->setCursor(15, midY - 3);
    m_canvas->printf("%d", m_sats);

    // Right, from the edge inwards: battery, then mute badge. The road name gets what is left.
    int rightX = LCD_WIDTH - 4;
    if (m_batPresent) rightX = drawBatteryIcon(rightX, midY) - 4;
    if (m_muted) {
        m_canvas->fillRoundRect(rightX - 14, midY - 6, 14, 13, 2, COLOR_ALERT);
        m_canvas->setFont(NULL);
        m_canvas->setTextSize(1);
        m_canvas->setTextColor(COLOR_WHITE);
        m_canvas->setCursor(rightX - 10, midY - 3);
        m_canvas->print("X");
        rightX -= 18;
    }

    // Center: road name (anti-aliased Vietnamese font), pixel-smooth marquee when too long
    const char* displayName = (m_roadName[0] != '\0') ? m_roadName : "VietHUD";
    const int boxX0 = 30, boxX1 = rightX;
    const int boxW = boxX1 - boxX0;
    const int baseline = 20;
    int textW = VnText::width(VN_FONT_MD, displayName);

    if (textW <= boxW) {
        VnText::draw(m_canvas, VN_FONT_MD, boxX0 + (boxW - textW) / 2, baseline, displayName, COLOR_ROAD_TEXT);
    } else {
        const uint32_t pauseStartMs = 1500, pauseEndMs = 1200;
        const uint32_t msPerPx = 28;   // ~35 px/s
        int maxOffset = textW - boxW;
        uint32_t scrollMs = (uint32_t)maxOffset * msPerPx;
        uint32_t elapsed = (millis() - m_marqueeStartTime) % (pauseStartMs + scrollMs + pauseEndMs);
        int offset = 0;
        if (elapsed >= pauseStartMs + scrollMs) {
            offset = maxOffset;
        } else if (elapsed > pauseStartMs) {
            offset = (elapsed - pauseStartMs) / msPerPx;
        }
        VnText::draw(m_canvas, VN_FONT_MD, boxX0 - offset, baseline, displayName, COLOR_ROAD_TEXT, boxX0, boxX1);
    }

    m_canvas->drawFastHLine(8, TOP_BAR_H, 224, 0x18E3);
}

// ---------------------------------------------------------------------------
// TRAFFIC ALERT ICON DRAWING PRIMITIVES (Centered at cx, cy)
// ---------------------------------------------------------------------------

void DisplayManager::drawIconCamera(int cx, int cy) {
    // Blue rectangular sign (48x40) centered at (cx, cy)
    m_canvas->fillRoundRect(cx - 24, cy - 20, 48, 40, 4, 0x027B); // Highway Blue
    m_canvas->drawRoundRect(cx - 24, cy - 20, 48, 40, 4, COLOR_WHITE);

    // White camera body
    m_canvas->fillRoundRect(cx - 18, cy - 8, 24, 16, 2, COLOR_WHITE);
    // Lens cone pointing right
    m_canvas->fillTriangle(cx + 6, cy - 5, cx + 17, cy - 10, cx + 17, cy + 10, COLOR_WHITE);
    m_canvas->fillTriangle(cx + 6, cy - 5, cx + 17, cy + 10, cx + 6, cy + 5, COLOR_WHITE);

    // Cyan lens reflection
    m_canvas->fillCircle(cx - 8, cy, 4, COLOR_CYAN);
    m_canvas->drawCircle(cx - 8, cy, 4, 0x0010);
    m_canvas->fillCircle(cx - 9, cy - 1, 1, COLOR_WHITE);

    // Mounting stem at bottom
    m_canvas->fillRect(cx - 12, cy + 8, 5, 8, COLOR_WHITE);
    m_canvas->fillRect(cx - 16, cy + 14, 13, 3, COLOR_WHITE);

    // Blinking red recording LED
    bool blink = (millis() / 350) % 2;
    if (blink) {
        m_canvas->fillCircle(cx + 2, cy - 5, 2, COLOR_ALERT);
    }
}

void DisplayManager::drawIconTrafficLight(int cx, int cy) {
    // Housing (28x46) centered at (cx, cy)
    m_canvas->fillRoundRect(cx - 14, cy - 23, 28, 46, 5, 0x10A2);
    m_canvas->drawRoundRect(cx - 14, cy - 23, 28, 46, 5, COLOR_SILVER);

    // Top: Active Red Lamp with glow & white reflection
    m_canvas->fillCircle(cx, cy - 14, 5, COLOR_ALERT);
    m_canvas->drawCircle(cx, cy - 14, 6, 0xC800);
    m_canvas->fillCircle(cx - 2, cy - 16, 1, COLOR_WHITE);

    // Middle: Inactive Yellow
    m_canvas->fillCircle(cx, cy, 4, 0x4200);

    // Bottom: Inactive Green
    m_canvas->fillCircle(cx, cy + 14, 4, 0x01E0);

    // Top visor
    m_canvas->drawFastHLine(cx - 8, cy - 23, 16, COLOR_WHITE);
}

void DisplayManager::drawIconResidentArea(int cx, int cy) {
    // Biển R.420: Nền xanh dương (50x40) centered at (cx, cy)
    m_canvas->fillRoundRect(cx - 25, cy - 20, 50, 40, 4, 0x027B);
    m_canvas->drawRoundRect(cx - 25, cy - 20, 50, 40, 4, COLOR_WHITE);

    // Tòa nhà cao tầng bên trái
    m_canvas->fillRect(cx - 19, cy - 9, 16, 25, COLOR_WHITE);
    m_canvas->fillRect(cx - 16, cy - 5, 3, 4, 0x027B);
    m_canvas->fillRect(cx - 10, cy - 5, 3, 4, 0x027B);
    m_canvas->fillRect(cx - 16, cy + 3, 3, 4, 0x027B);
    m_canvas->fillRect(cx - 10, cy + 3, 3, 4, 0x027B);

    // Ngôi nhà mái nhọn bên phải
    m_canvas->fillTriangle(cx + 1, cy - 2, cx + 11, cy - 10, cx + 21, cy - 2, COLOR_WHITE);
    m_canvas->fillRect(cx + 2, cy - 2, 17, 18, COLOR_WHITE);
    m_canvas->fillRect(cx + 8, cy + 5, 5, 11, 0x027B);
}

void DisplayManager::drawIconNoOvertaking(int cx, int cy) {
    // Biển P.125: Hình tròn viền đỏ nền trắng (r=20) centered at (cx, cy)
    for (int i = 0; i < 4; i++) {
        m_canvas->drawCircle(cx, cy, 20 - i, COLOR_SIGN_RED);
    }
    m_canvas->fillCircle(cx, cy, 16, COLOR_WHITE);

    // Xe đen bên phải (đi bình thường)
    m_canvas->fillRoundRect(cx + 2, cy - 6, 8, 11, 2, COLOR_BLACK);
    m_canvas->fillRect(cx + 4, cy - 3, 4, 4, 0x7BEF);

    // Xe đỏ bên trái (xe vượt cấm)
    m_canvas->fillRoundRect(cx - 10, cy - 6, 8, 11, 2, COLOR_ALERT);
    m_canvas->fillRect(cx - 8, cy - 3, 4, 4, COLOR_WHITE);
}

void DisplayManager::drawIconTollBooth(int cx, int cy) {
    // Biển Trạm thu phí BOT (50x40) centered at (cx, cy)
    m_canvas->fillRoundRect(cx - 25, cy - 20, 50, 40, 4, 0xFDE0);
    m_canvas->drawRoundRect(cx - 25, cy - 20, 50, 40, 4, COLOR_SIGN_RED);
    m_canvas->drawRoundRect(cx - 24, cy - 19, 48, 38, 3, COLOR_SIGN_RED);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_BLACK);
    m_canvas->setCursor(cx - 18, cy - 10);
    m_canvas->print("BOT");

    // Barrier arm at bottom
    for (int i = 0; i < 36; i += 8) {
        m_canvas->fillRect(cx - 18 + i, cy + 11, 4, 4, COLOR_ALERT);
        m_canvas->fillRect(cx - 14 + i, cy + 11, 4, 4, COLOR_WHITE);
    }
}

void DisplayManager::drawIconDanger(int cx, int cy) {
    // Biển W.208: Tam giác viền đỏ nền vàng dấu ! centered at (cx, cy)
    int x1 = cx,      y1 = cy - 20;
    int x2 = cx - 22, y2 = cy + 17;
    int x3 = cx + 22, y3 = cy + 17;

    m_canvas->fillTriangle(x1, y1 + 3, x2 + 3, y2 - 2, x3 - 3, y3 - 2, 0xFFE0);

    for (int i = 0; i < 3; i++) {
        m_canvas->drawTriangle(x1, y1 + i, x2 + i, y2 - i, x3 - i, y3 - i, COLOR_SIGN_RED);
    }

    m_canvas->fillRect(cx - 2, cy - 9, 4, 13, COLOR_BLACK);
    m_canvas->fillRect(cx - 2, cy + 8, 4, 4, COLOR_BLACK);
}

// ---------------------------------------------------------------------------
// LEFT ALERT CARD (Bên trái: Biển báo phía trên, Khoảng cách phía dưới)
// ---------------------------------------------------------------------------
void DisplayManager::drawAlertLeftCard() {
    int boxX = 6;
    int boxY = 28;
    int boxW = 98;
    int boxH = 142;
    int cx = boxX + boxW / 2; // cx = 55

    uint16_t alertColor = COLOR_WHITE;
    const char* alertTitle = "";

    switch (m_alertType) {
        case 1:
            alertColor = COLOR_ALERT;
            alertTitle = L("Bắn tốc độ", "Speed cam");
            break;
        case 4:
            alertColor = COLOR_ALERT;
            alertTitle = L("Phạt nguội", "Camera");
            break;
        case 6:
            alertColor = COLOR_WARN;
            alertTitle = L("Đèn đỏ", "Red light");
            break;
        case 2:
            alertColor = COLOR_CYAN;
            alertTitle = L("Dân cư", "Residential");
            break;
        case 3:
            alertColor = COLOR_ALERT;
            alertTitle = L("Cấm vượt", "No passing");
            break;
        case 5:
            alertColor = COLOR_WARN;
            alertTitle = L("Trạm BOT", "Toll");
            break;
        case 10:
        default:
            alertColor = COLOR_WARN;
            alertTitle = L("Nguy hiểm", "Danger");
            break;
    }

    // Card background & double border
    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 8, alertColor);
    m_canvas->drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 7, alertColor);

    // 1. Phía trên: Biển báo hiệu (Icon centered at cx = 61, cy = 56)
    switch (m_alertType) {
        case 1:
        case 4:
            drawIconCamera(cx, 56);
            break;
        case 6:
            drawIconTrafficLight(cx, 56);
            break;
        case 2:
            drawIconResidentArea(cx, 56);
            break;
        case 3:
            drawIconNoOvertaking(cx, 56);
            break;
        case 5:
            drawIconTollBooth(cx, 56);
            break;
        case 10:
        default:
            drawIconDanger(cx, 56);
            break;
    }

    // 2. Alert Title Tag
    VnText::drawCentered(m_canvas, VN_FONT_SM, cx, 97, alertTitle, alertColor);

    // 3. Phía dưới: Khoảng cách đếm lùi to rõ (FreeSansBold18pt7b)
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

    int totalW = w + 10;
    int startDistX = cx - totalW / 2;
    int distY = 124;

    m_canvas->setCursor(startDistX, distY);
    m_canvas->print(distStr);

    // Unit "m"
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_SILVER);
    m_canvas->setCursor(startDistX + w + 3, distY - 14);
    m_canvas->print("m");

    // 4. Countdown Progress Bar at bottom of card
    int barW = 86;
    int barH = 4;
    int barX = cx - barW / 2;
    int barY = 148;

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
// SPEED LIMIT SIGN (Biển báo tốc độ giới hạn)
// When centered (no alert): cx=120, cy=98, radius=54 (TO VÀ RÕ NHẤT)
// When shifted right (alert active): cx=176, cy=98, radius=46 (VẪN RẤT TO VÀ RÕ)
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

    // Outer subtle contrast halo
    m_canvas->drawCircle(cx, cy, radius + 1, 0x18C3);

    // Thick official red border (8px to 10px thick)
    int borderThick = (radius >= 60) ? 10 : 8;
    for (int r = radius; r >= radius - borderThick; r--) {
        m_canvas->drawCircle(cx, cy, r, outerBorderColor);
    }

    // Pure white center disk
    m_canvas->fillCircle(cx, cy, radius - (borderThick + 1), innerBgColor);

    char buf[12];
    if (limit <= 0) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%d", limit);
    }

    // Ultra-large font selection tailored to sign radius & digit count:
    const GFXfont* fontToUse = nullptr;
    if (radius >= 60) {
        // Centered sign (R=66, diameter 132px)
        if (limit >= 100) {
            fontToUse = &SpeedSignBold52pt; // 3 digits: "100", "120" (37px tall)
        } else {
            fontToUse = &SpeedSignBold72pt; // 2 digits: "50", "60", "80", "--" (53px tall, fills 87% of white circle)
        }
    } else {
        // Shifted sign when alert is active (R=54, diameter 108px)
        if (limit >= 100) {
            fontToUse = &SpeedSignBold42pt; // 3 digits: "100", "120" (30px tall)
        } else {
            fontToUse = &SpeedSignBold60pt; // 2 digits: "50", "60", "80", "--" (44px tall, fills 88% of white circle)
        }
    }
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
// CURRENT SPEED SECTION (Cockpit Vehicle Speed at bottom: cx=120, cy=204, h=54)
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

    // CPU temperature, bottom right (sampled every 2 s)
    static uint32_t lastTempMs = 0;
    static float cpuTemp = 0;
    if (lastTempMs == 0 || millis() - lastTempMs >= 2000) {
        lastTempMs = millis();
        cpuTemp = temperatureRead();
    }
    char tempStr[12];
    snprintf(tempStr, sizeof(tempStr), "%d°C", (int)lroundf(cpuTemp));
    VnText::drawRight(m_canvas, VN_FONT_SM, cardX + cardW - 12, barY - 5, tempStr,
                      cpuTemp >= 75.0f ? COLOR_ALERT : COLOR_SILVER);
}

void DisplayManager::drawToast() {
    if (millis() < m_toastExpiry && m_toastText[0] != '\0') {
        int textW = VnText::width(VN_FONT_LG, m_toastText);
        int boxW = textW + 32;
        if (boxW > 232) boxW = 232;
        int boxH = 38;
        int boxX = 120 - boxW / 2;
        int boxY = 78;

        m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_TOAST_BG);
        m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 8, m_toastColor);
        m_canvas->drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 7, m_toastColor);

        VnText::drawCentered(m_canvas, VN_FONT_LG, 120, boxY + 26, m_toastText, m_toastColor);
    }
}

// ---------------------------------------------------------------------------
// DEDICATED FULL-SCREEN WIFI & OTA PAGE
// ---------------------------------------------------------------------------
void DisplayManager::drawWiFiOTAPage() {
    m_canvas->fillRect(0, 0, LCD_WIDTH, 30, COLOR_BG);
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_CYAN);
    m_canvas->setCursor(54, 8);
    m_canvas->print("WIFI & OTA");

    m_canvas->drawFastHLine(8, 30, 224, COLOR_CARD_BORDER);

    uint16_t cardBorderColor = COLOR_CYAN;
    if (m_otaWorkflowState == 5) {
        cardBorderColor = COLOR_SAFE;
    } else if (m_otaWorkflowState == 4 || m_otaWorkflowState == 7) {
        cardBorderColor = COLOR_WARN;
    } else if (m_otaWorkflowState == 6) {
        cardBorderColor = COLOR_ALERT;
    }

    int boxX = 8, boxY = 36, boxW = 224, boxH = 158;
    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 8, cardBorderColor);
    m_canvas->drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 7, cardBorderColor);

    int cx = 120;

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
    } else { // FAILED
        m_canvas->fillTriangle(cx, 50, cx - 18, 84, cx + 18, 84, COLOR_ALERT);
        m_canvas->fillTriangle(cx, 53, cx - 15, 82, cx + 15, 82, COLOR_WHITE);
        m_canvas->fillRect(cx - 1, 60, 3, 12, COLOR_BLACK);
        m_canvas->fillRect(cx - 1, 75, 3, 3, COLOR_BLACK);
    }

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(cardBorderColor);

    const char* titleMsg = m_otaWorkflowMsg[0] != '\0' ? m_otaWorkflowMsg : L("DANG XU LY...", "PROCESSING...");
    int textW = strlen(titleMsg) * 6;
    int textX = cx - textW / 2;
    if (textX < boxX + 6) textX = boxX + 6;
    m_canvas->setCursor(textX, 98);
    m_canvas->print(titleMsg);

    if (m_otaWorkflowState == 1) { // CONNECTING
        m_canvas->setTextColor(COLOR_SILVER);
        m_canvas->setCursor(boxX + 16, 122);
        m_canvas->print("1. iPhone cua Pham");
        m_canvas->setCursor(boxX + 16, 138);
        m_canvas->print("2. VuPQ");
        m_canvas->setTextColor(COLOR_CYAN);
        m_canvas->setCursor(boxX + 16, 158);
        m_canvas->print(L("Dang do tim tin hieu...", "Searching for signal..."));
    } else if (m_otaWorkflowState == 2 || m_otaWorkflowState == 3) { // CONNECTED / CHECKING
        m_canvas->setTextColor(COLOR_WHITE);
        m_canvas->setCursor(boxX + 16, 114);
        m_canvas->printf("IP: %s", m_wifiIp);

        m_canvas->setTextColor(COLOR_SAFE);
        m_canvas->setCursor(boxX + 16, 128);
        m_canvas->printf(L("Song: %d dBm (Tot)", "Signal: %d dBm (Good)"), m_wifiRssi);

        m_canvas->setTextColor(COLOR_CYAN);
        m_canvas->setCursor(boxX + 16, 144);
        m_canvas->print("Pi4 LAN: " PI4_LAN_IP);

        m_canvas->setTextColor(COLOR_ROAD_TEXT);
        m_canvas->setCursor(boxX + 16, 160);
        m_canvas->print("Pi4 TS:  " PI4_TAILSCALE_IP);
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
        m_canvas->print(L("KHONG NGAT NGUON !", "DO NOT POWER OFF !"));
    } else if (m_otaWorkflowState == 5) { // UP_TO_DATE
        m_canvas->setTextColor(COLOR_SAFE);
        m_canvas->setCursor(boxX + 16, 114);
        m_canvas->print(L("Phien ban: v" FW_VERSION "", "Version: v" FW_VERSION ""));

        m_canvas->setTextColor(COLOR_CYAN);
        m_canvas->setCursor(boxX + 16, 128);
        m_canvas->printf("Web OTA: http://%s/", m_wifiIp);

        m_canvas->setTextColor(COLOR_ROAD_TEXT);
        m_canvas->setCursor(boxX + 16, 144);
        m_canvas->print("Pi4 TS: " PI4_TAILSCALE_IP);

        m_canvas->setTextColor(COLOR_WARN);
        m_canvas->setCursor(boxX + 16, 162);
        m_canvas->printf(L("Tu dong ve HUD sau: %ds", "Back to HUD in: %ds"), m_otaCountdownSec);
    } else { // FAILED
        m_canvas->setTextColor(COLOR_SILVER);
        m_canvas->setCursor(boxX + 16, 114);
        m_canvas->print(L("Khong tim thay AP / Pi 4", "No AP / Pi 4 found"));
        m_canvas->setCursor(boxX + 16, 128);
        m_canvas->print(L("Thu ca LAN & Tailscale", "Tried LAN & Tailscale"));
        m_canvas->setTextColor(COLOR_ROAD_TEXT);
        m_canvas->setCursor(boxX + 16, 144);
        m_canvas->print("TS IP: " PI4_TAILSCALE_IP);

        m_canvas->setTextColor(COLOR_WARN);
        m_canvas->setCursor(boxX + 16, 162);
        m_canvas->printf(L("Tu dong ve HUD sau: %ds", "Back to HUD in: %ds"), m_otaCountdownSec);
    }

    m_canvas->fillRoundRect(16, 202, 208, 28, 6, COLOR_CARD_BG);
    m_canvas->drawRoundRect(16, 202, 208, 28, 6, COLOR_CARD_BORDER);

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_WHITE);
    m_canvas->setCursor(32, 211);
    m_canvas->print(L("[ NUT GIUA: VE TRANG HUD ]", "[ MID BUTTON: BACK TO HUD ]"));
}

void DisplayManager::render() {
    if (!m_canvas) return;

    // Clear frame to deep black
    m_canvas->fillScreen(COLOR_BG);

    // If WiFi is enabled, switch to dedicated WiFi & OTA screen!
    if (m_wifiEnabled) {
        drawWiFiOTAPage();
        m_canvas->flush();
        return;
    }

    // Normal Driving HUD Dashboard
    // 1. Top Bar with Road Name & GPS sats
    drawTopBar();

    // 2. Main Middle Area:
    if (m_alertActive && m_alertType != 0) {
        // STATE 2: Alert ACTIVE -> Left card (Icon top, Distance bottom), Right speed limit sign (r=54, diameter=108px)
        drawAlertLeftCard();
        drawSpeedLimitSign(172, 99, 54, m_speedLimit);
    } else {
        // STATE 1: NO alert -> Speed limit sign CENTERED and LARGEST (r=66, diameter=132px)
        drawSpeedLimitSign(120, 99, 66, m_speedLimit);
    }

    // 3. Cockpit Vehicle Speed Section at bottom (cx=120, cy=204, h=54)
    drawCurrentSpeedSection(120, 204, m_speed, m_speedLimit);

    // 4. Toast Popup if active
    drawToast();

    // Flush entire frame buffer to ST7789 via high-speed SPI
    m_canvas->flush();
}

void DisplayManager::renderSettings(uint8_t selectedIdx, bool isEditing) {
    if (!m_canvas) return;

    m_canvas->fillScreen(COLOR_BG);

    // 1. Header bar: title + position
    const int headerH = 34;
    m_canvas->fillRect(0, 0, LCD_WIDTH, headerH, COLOR_CARD_BG);
    m_canvas->drawFastHLine(0, headerH, LCD_WIDTH, COLOR_CARD_BORDER);
    VnText::draw(m_canvas, VN_FONT_LG, 12, 25, L("Cài đặt", "Settings"), COLOR_CYAN);
    char idxBuf[16];
    snprintf(idxBuf, sizeof(idxBuf), "%d / %d", selectedIdx + 1, SETTING_COUNT);
    VnText::drawRight(m_canvas, VN_FONT_SM, 226, 23, idxBuf, COLOR_SILVER);

    // 2. List: 3 large two-line rows (name on top, value below on the right)
    const int VISIBLE_ITEMS = 3;
    int topIdx = selectedIdx - (VISIBLE_ITEMS - 1) / 2;
    if (topIdx + VISIBLE_ITEMS > SETTING_COUNT) topIdx = SETTING_COUNT - VISIBLE_ITEMS;
    if (topIdx < 0) topIdx = 0;

    const int listY = 39;
    const int rowH = 54;
    const int rowGap = 4;
    const int rowX = 6;
    const int rowW = 222;

    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int itemIdx = topIdx + i;
        if (itemIdx >= SETTING_COUNT) break;

        bool isSel = (itemIdx == selectedIdx);
        bool isAction = settings.isActionItem(itemIdx);
        int y = listY + i * (rowH + rowGap);
        uint16_t accent = isSel ? (isEditing ? COLOR_WARN : COLOR_CYAN) : COLOR_CARD_BORDER;

        m_canvas->fillRoundRect(rowX, y, rowW, rowH, 8, isSel ? 0x10A2 : COLOR_CARD_BG);
        m_canvas->drawRoundRect(rowX, y, rowW, rowH, 8, accent);
        if (isSel) {
            m_canvas->drawRoundRect(rowX + 1, y + 1, rowW - 2, rowH - 2, 7, accent);
            m_canvas->fillRoundRect(rowX + 5, y + 9, 4, rowH - 18, 2, accent);   // selection bar
        }

        // Setting name
        VnText::draw(m_canvas, VN_FONT_MD, rowX + 16, y + 22, settings.getItemName(itemIdx),
                     isSel ? COLOR_WHITE : COLOR_SILVER);

        // Value badge
        char valBuf[32];
        settings.getItemValueStr(itemIdx, valBuf, sizeof(valBuf));
        char displayVal[40];
        if (isSel && isEditing && !isAction) {
            snprintf(displayVal, sizeof(displayVal), "‹ %s ›", valBuf);
        } else {
            snprintf(displayVal, sizeof(displayVal), "%s", valBuf);
        }

        // Selected value in the large font, unless it would not fit the row
        bool large = isSel && VnText::width(VN_FONT_LG, displayVal) + 18 <= rowW - 16;
        const VnFont& valFont = large ? VN_FONT_LG : VN_FONT_MD;
        int valW = VnText::width(valFont, displayVal);
        int badgeW = valW + 18;
        int badgeH = 24;
        int badgeX = rowX + rowW - badgeW - 8;
        int badgeY = y + rowH - badgeH - 5;

        uint16_t badgeBg = isSel ? (isEditing ? 0x4200 : (isAction ? 0x0320 : 0x027B)) : 0x18C3;
        uint16_t valColor = isSel ? (isEditing ? COLOR_WARN : COLOR_WHITE) : COLOR_CYAN;
        m_canvas->fillRoundRect(badgeX, badgeY, badgeW, badgeH, 6, badgeBg);
        if (isSel) m_canvas->drawRoundRect(badgeX, badgeY, badgeW, badgeH, 6, accent);
        VnText::draw(m_canvas, valFont, badgeX + 9, badgeY + (large ? 19 : 18), displayVal, valColor);
    }

    // Scrollbar
    const int trackX = 234, trackY = listY, trackH = VISIBLE_ITEMS * (rowH + rowGap) - rowGap;
    m_canvas->drawFastVLine(trackX, trackY, trackH, COLOR_TRACK);
    int thumbH = (trackH * VISIBLE_ITEMS) / SETTING_COUNT;
    int thumbY = trackY + (selectedIdx * (trackH - thumbH)) / (SETTING_COUNT - 1);
    m_canvas->fillRoundRect(trackX - 1, thumbY, 3, thumbH, 1, isEditing ? COLOR_WARN : COLOR_CYAN);

    // 3. Footer hint
    const int footerY = 214;
    m_canvas->fillRect(0, footerY, LCD_WIDTH, LCD_HEIGHT - footerY, COLOR_CARD_BG);
    m_canvas->drawFastHLine(0, footerY, LCD_WIDTH, COLOR_CARD_BORDER);
    if (isEditing) {
        VnText::drawCentered(m_canvas, VN_FONT_SM, 120, 232, L("Trái/Phải: chỉnh  •  Giữa: xong", "Left/Right: adjust  •  Mid: done"), COLOR_WARN);
    } else {
        VnText::drawCentered(m_canvas, VN_FONT_SM, 120, 232, L("Giữa: chọn  •  Giữ: lưu & thoát", "Mid: select  •  Hold: save & exit"), COLOR_SILVER);
    }

    m_canvas->flush();
}

void DisplayManager::renderSystemInfo(const SystemInfoData& info) {
    if (!m_canvas) return;

    // Static facts are read once; live values refresh every 500 ms.
    static uint32_t sketchSize = 0, appSlot = 0;
    if (sketchSize == 0) {
        sketchSize = ESP.getSketchSize();
        appSlot = ESP.getFreeSketchSpace();   // size of the OTA slot the app must fit in
    }

    const int ROWS = 10;
    static char values[ROWS][40];
    static uint32_t lastUpdate = 0;
    static uint8_t lastLang = 0xFF;
    if (lastUpdate == 0 || millis() - lastUpdate >= 500 || lastLang != settings.data.language) {
        lastUpdate = millis();
        lastLang = settings.data.language;
        uint32_t up = millis() / 1000;
        float t = temperatureRead();

        snprintf(values[0], 40, "v%s (%.8s)", FW_VERSION, FW_BUILD);
        if (!info.batteryPresent) {
            snprintf(values[1], 40, "%s", L("Không có pin (USB)", "No battery (USB)"));
        } else {
            snprintf(values[1], 40, "%d%%%s", info.batteryLevel,
                     info.batteryCharging ? L(" · đang sạc", " · charging") : "");
        }
        snprintf(values[2], 40, "S3 · %lu MHz · %d°C", (unsigned long)ESP.getCpuFreqMHz(), (int)lroundf(t));
        snprintf(values[3], 40, L("Trống %lu / %lu KB", "Free %lu / %lu KB"),
                 (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getHeapSize() / 1024));
        snprintf(values[4], 40, L("Trống %.1f / %.1f MB", "Free %.1f / %.1f MB"),
                 ESP.getFreePsram() / 1048576.0f, ESP.getPsramSize() / 1048576.0f);
        snprintf(values[5], 40, "%lu MB · app %.1f / %.1f MB", (unsigned long)(ESP.getFlashChipSize() / 1048576),
                 sketchSize / 1048576.0f, appSlot / 1048576.0f);
        if (info.gpsFix) {
            snprintf(values[6], 40, L("%d vệ tinh · ±%dm · %ds", "%d sats · ±%dm · %ds"),
                     info.gpsSats, (int)lroundf(info.gpsHAccM), (int)((info.gpsTtffMs + 500) / 1000));
        } else {
            snprintf(values[6], 40, L("%s %lu · chưa định vị", "%s %lu · no fix"), info.gpsProtocol,
                     (unsigned long)info.gpsBaud);
        }
        snprintf(values[7], 40, L("%lu điểm", "%lu points"), (unsigned long)info.alertCount);
        snprintf(values[8], 40, L("%lu tên", "%lu names"), (unsigned long)info.roadNameCount);
        snprintf(values[9], 40, "%02lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
                 (unsigned long)(up % 60));
    }
    const char* labels[ROWS] = {
        L("Phần mềm", "Firmware"), L("Pin", "Battery"), "CPU", "RAM", "PSRAM", "Flash", "GPS",
        L("Cảnh báo", "Alerts"), L("Tên đường", "Road names"), L("Hoạt động", "Uptime"),
    };

    m_canvas->fillScreen(COLOR_BG);

    const int headerH = 34;
    m_canvas->fillRect(0, 0, LCD_WIDTH, headerH, COLOR_CARD_BG);
    m_canvas->drawFastHLine(0, headerH, LCD_WIDTH, COLOR_CARD_BORDER);
    VnText::draw(m_canvas, VN_FONT_LG, 12, 25, L("Thông tin hệ thống", "System info"), COLOR_CYAN);

    const int firstBase = 53, step = 17;
    for (int i = 0; i < ROWS; i++) {
        int base = firstBase + i * step;
        if (i % 2 == 0) m_canvas->fillRect(4, base - 13, LCD_WIDTH - 8, step, 0x0841);
        VnText::draw(m_canvas, VN_FONT_SM, 10, base, labels[i], COLOR_SILVER);
        VnText::drawRight(m_canvas, VN_FONT_SM, LCD_WIDTH - 10, base, values[i], COLOR_WHITE);
    }

    const int footerY = 214;
    m_canvas->fillRect(0, footerY, LCD_WIDTH, LCD_HEIGHT - footerY, COLOR_CARD_BG);
    m_canvas->drawFastHLine(0, footerY, LCD_WIDTH, COLOR_CARD_BORDER);
    VnText::drawCentered(m_canvas, VN_FONT_SM, 120, 232, L("Bấm nút bất kỳ để quay lại", "Press any button to go back"),
                         COLOR_SILVER);

    m_canvas->flush();
}

void DisplayManager::showOtaProgress(const char* title, int progress, const char* detail) {
    if (!m_canvas) return;

    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    m_canvas->fillScreen(COLOR_BG);

    // Header bar (y: 0 to 32)
    m_canvas->fillRect(0, 0, LCD_WIDTH, 32, 0x0842);
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_CYAN);
    m_canvas->setCursor(34, 8);
    m_canvas->print(L("DANG CAP NHAT", "UPDATING"));
    m_canvas->drawFastHLine(0, 32, 240, COLOR_CARD_BORDER);

    // Main Card (y: 42 to 226)
    int boxX = 10;
    int boxY = 42;
    int boxW = 220;
    int boxH = 186;
    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CYAN);

    // Title / Firmware name
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_WHITE);
    int titleW = (title ? strlen(title) : 0) * 6;
    m_canvas->setCursor(120 - titleW / 2, boxY + 14);
    if (title) m_canvas->print(title);

    // Large Percentage Digits (FreeSansBold24pt7b)
    char pctStr[12];
    snprintf(pctStr, sizeof(pctStr), "%d%%", progress);
    m_canvas->setFont(&FreeSansBold24pt7b);
    m_canvas->setTextColor(COLOR_SAFE);

    int16_t x1, y1;
    uint16_t w, h;
    m_canvas->getTextBounds(pctStr, 0, 0, &x1, &y1, &w, &h);
    m_canvas->setCursor(120 - (w / 2) - x1, boxY + 70);
    m_canvas->print(pctStr);

    // Progress Bar (Sleek, glowing, with track)
    int barX = boxX + 16;
    int barY = boxY + 92;
    int barW = boxW - 32;
    int barH = 14;

    m_canvas->fillRoundRect(barX, barY, barW, barH, 4, COLOR_TRACK);
    m_canvas->drawRoundRect(barX, barY, barW, barH, 4, COLOR_CARD_BORDER);

    int innerW = barW - 4;
    int fillW = (progress * innerW) / 100;
    if (fillW > 0) {
        m_canvas->fillRoundRect(barX + 2, barY + 2, fillW, barH - 4, 3, COLOR_SAFE);
    }

    // Detail text / Source
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(1);
    if (detail && detail[0] != '\0') {
        m_canvas->setTextColor(COLOR_ROAD_TEXT);
        int detW = strlen(detail) * 6;
        m_canvas->setCursor(120 - detW / 2, barY + 22);
        m_canvas->print(detail);
    }

    // Warning
    m_canvas->setTextColor(COLOR_ALERT);
    m_canvas->setCursor(34, boxY + 142);
    m_canvas->print(L("! KHONG DUOC TAT NGUON !", "!  DO NOT POWER OFF  !"));

    m_canvas->setTextColor(COLOR_SILVER);
    m_canvas->setCursor(44, boxY + 160);
    m_canvas->print(L("Dang ghi bo nho Flash...", "Writing flash memory..."));

    m_canvas->flush();
}

void DisplayManager::showOtaSuccess(const char* version, int countdownSec) {
    if (!m_canvas) return;

    m_canvas->fillScreen(COLOR_BG);

    // Header bar (y: 0 to 32)
    m_canvas->fillRect(0, 0, LCD_WIDTH, 32, 0x0320); // Dark emerald green header
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_SAFE);
    m_canvas->setCursor(28, 8);
    m_canvas->print(L("CAP NHAT XONG !", "UPDATE DONE !"));
    m_canvas->drawFastHLine(0, 32, 240, COLOR_SAFE);

    // Main Card
    int boxX = 10;
    int boxY = 42;
    int boxW = 220;
    int boxH = 186;
    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_SAFE);
    m_canvas->drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 7, COLOR_SAFE);

    // Glowing Green Circle with Checkmark Icon
    int iconX = 120;
    int iconY = boxY + 42;
    m_canvas->fillCircle(iconX, iconY, 26, COLOR_SAFE);
    m_canvas->fillCircle(iconX, iconY, 22, COLOR_CARD_BG);

    // Bold Checkmark lines
    for (int t = -1; t <= 1; t++) {
        m_canvas->drawLine(iconX - 11, iconY + t, iconX - 3, iconY + 8 + t, COLOR_SAFE);
        m_canvas->drawLine(iconX - 3, iconY + 8 + t, iconX + 12, iconY - 8 + t, COLOR_SAFE);
    }

    // Success Title
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_SAFE);
    int titleW = 11 * 12;
    m_canvas->setCursor(120 - titleW / 2, boxY + 80);
    m_canvas->print(L("THANH CONG!", "SUCCESS!"));

    // New version string
    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_WHITE);
    char verBuf[48];
    snprintf(verBuf, sizeof(verBuf), L("Phien ban moi: v%s", "New version: v%s"), (version ? version : FW_VERSION));
    int verW = strlen(verBuf) * 6;
    m_canvas->setCursor(120 - verW / 2, boxY + 110);
    m_canvas->print(verBuf);

    // Subtext
    m_canvas->setTextColor(COLOR_CYAN);
    m_canvas->setCursor(34, boxY + 128);
    m_canvas->print(L("Da cap nhat tinh nang moi", "New features installed"));

    // Countdown before reboot
    char countBuf[48];
    if (countdownSec > 0) {
        snprintf(countBuf, sizeof(countBuf), L("Khoi dong lai sau: %ds...", "Restarting in: %ds..."), countdownSec);
    } else {
        snprintf(countBuf, sizeof(countBuf), "%s", L("Dang khoi dong lai...", "Restarting..."));
    }
    m_canvas->setTextColor(COLOR_WARN);
    int cW = strlen(countBuf) * 6;
    m_canvas->setCursor(120 - cW / 2, boxY + 154);
    m_canvas->print(countBuf);

    m_canvas->flush();
}

void DisplayManager::showOtaFailure(const char* reason) {
    if (!m_canvas) return;

    m_canvas->fillScreen(COLOR_BG);

    // Header bar
    m_canvas->fillRect(0, 0, LCD_WIDTH, 32, 0x6000); // Dark red header
    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_ALERT);
    m_canvas->setCursor(24, 8);
    m_canvas->print(L("CAP NHAT LOI !", "UPDATE ERROR !"));
    m_canvas->drawFastHLine(0, 32, 240, COLOR_ALERT);

    // Main Card
    int boxX = 10;
    int boxY = 42;
    int boxW = 220;
    int boxH = 186;
    m_canvas->fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_CARD_BG);
    m_canvas->drawRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_ALERT);

    // Red X icon
    int iconX = 120;
    int iconY = boxY + 42;
    m_canvas->fillCircle(iconX, iconY, 26, COLOR_ALERT);
    m_canvas->fillCircle(iconX, iconY, 22, COLOR_CARD_BG);

    for (int t = -1; t <= 1; t++) {
        m_canvas->drawLine(iconX - 9 + t, iconY - 9, iconX + 9 + t, iconY + 9, COLOR_ALERT);
        m_canvas->drawLine(iconX - 9 + t, iconY + 9, iconX + 9 + t, iconY - 9, COLOR_ALERT);
    }

    m_canvas->setFont(NULL);
    m_canvas->setTextSize(2);
    m_canvas->setTextColor(COLOR_ALERT);
    int titleW = 8 * 12;
    m_canvas->setCursor(120 - titleW / 2, boxY + 80);
    m_canvas->print(L("THAT BAI", "FAILED"));

    m_canvas->setTextSize(1);
    m_canvas->setTextColor(COLOR_WHITE);
    if (reason && reason[0] != '\0') {
        int rW = strlen(reason) * 6;
        m_canvas->setCursor(120 - rW / 2, boxY + 112);
        m_canvas->print(reason);
    }

    m_canvas->setTextColor(COLOR_SILVER);
    m_canvas->setCursor(34, boxY + 134);
    m_canvas->print(L("Kiem tra lai Pi 4 / WiFi", "Check Pi 4 / WiFi"));

    m_canvas->setTextColor(COLOR_WARN);
    m_canvas->setCursor(44, boxY + 158);
    m_canvas->print(L("Tu dong quay lai HUD...", "Returning to HUD..."));

    m_canvas->flush();
}
