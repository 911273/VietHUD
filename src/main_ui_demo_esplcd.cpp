// radar_car — UI verification demo (SIMULATED DATA, no real sensors)
//
// No radar / GNSS hardware is connected yet. This build exercises the
// Dashboard layout, target rendering, TTC/risk color coding, the
// audio-gate hysteresis display, the radar/GNSS fail-safe states, and now
// a Settings screen (spec sections 16-17, 33-34, 52-61) — all driven by a
// synthetic data generator instead of real sensors, purely so the LVGL UI
// can be checked on real hardware before Phase 2 (real radar UART) exists.
//
// Settings changes have real, visible effect on this demo (they're not a
// dead mockup): TTC thresholds recolor targets live, max range rescales
// target position, max targets/min speed filter the simulated target set,
// the audio-gate hysteresis pair drives the Audio status dot, and the
// Brightness slider drives the *real* backlight via LEDC PWM. Settings
// persist to NVS (ESP32 Preferences) across reboots.
//
// Not implemented from the spec (out of scope for a sensor-less UI demo):
// Basic/Advanced gating with long-press (section 17) — nothing here is
// safety-critical yet, so it's left as one flat Settings screen. Radar
// direction/SNR/trigger-count and Lane-model settings (sections 16.1,
// 16.4) aren't included because this demo has no lane-corridor model.
//
// This is NOT the Phase 2 radar integration. No real UART parsing, no
// real TTC safety engine, no real buzzer.
//
// esp_lcd variant: uses the official Espressif esp_lcd_axs15231b component
// (lib/esp_lcd_axs15231b) instead of Arduino_GFX, which allows real partial-
// area screen writes and correct MADCTL rotation — see the notes further
// down and docs/V1.2_hardening_proposal.md "Lỗi #4". Requires Arduino-ESP32
// 3.x / IDF 5.x — build with: pio run -e uidemo3 -t upload
// (The Arduino_GFX version for the older platform is src/main_ui_demo.cpp,
// built with: pio run -e uidemo -t upload — keep it working as a fallback.)

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include <Preferences.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_spi.h>
#include "esp_lcd_axs15231b.h"

#include "pincfg.h"
#include "dispcfg.h"
#include "AXS15231BTouch.h"

// ---------------------------------------------------------------------
// Display (esp_lcd + the official Espressif AXS15231B component) + touch
// (unchanged — our own I2C driver, independent of the display stack).
//
// This replaces the Arduino_GFX + Canvas workaround used on env:uidemo (see
// docs/V1.2_hardening_proposal.md "Lỗi phần cứng driver AXS15231B" / "Lỗi #4"
// for why that path is stuck at whole-frame-only flushes). esp_lcd's
// panel_axs15231b_draw_bitmap() builds the CASET/RAMWR(C) sequence for the
// literal x/y/w/h it's given — real arbitrary-region writes — and
// panel_mirror/swap_xy set MADCTL directly and correctly, so rotation no
// longer needs the Canvas workaround either. Requires IDF 5.x (env:uidemo3
// / pioarduino only — this file will not compile against env:uidemo's
// IDF 4.4.7 headers, which is why it's a separate .cpp/env).
// DIAGNOSTIC: portrait/native for now (no swap_xy/mirror) to isolate basic
// drawing from rotation — same divide-and-conquer that found the Arduino_GFX
// rotation bug earlier. Landscape 480x320 is the real target once this works.
#define LS_W 320
#define LS_H 480

static esp_lcd_panel_io_handle_t lcdIo;
static esp_lcd_panel_handle_t lcdPanel;

static void initDisplay() {
    // Not using AXS15231B_PANEL_BUS_QSPI_CONFIG here: its designated-initializer
    // order doesn't match spi_bus_config_t's declaration order, which C++
    // (unlike C) requires — fails to compile. Same field values, set by hand.
    spi_bus_config_t busCfg = {};
    busCfg.sclk_io_num = TFT_SCK;
    busCfg.data0_io_num = TFT_SDA0;
    busCfg.data1_io_num = TFT_SDA1;
    busCfg.data2_io_num = TFT_SDA2;
    busCfg.data3_io_num = TFT_SDA3;
    busCfg.max_transfer_sz = LS_W * 20 * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &busCfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t ioCfg = {};
    ioCfg.cs_gpio_num = TFT_CS;
    ioCfg.dc_gpio_num = -1;
    // Reverted to the vendor macro's exact values (spi_mode=3, 40MHz).
    // Comparing to Arduino_ESP32QSPI's SPI_MODE0 was invalid — that path
    // uses raw spi_device transactions with an explicit 8-bit cmd + 24-bit
    // addr phase split; esp_lcd_panel_io_spi's single 32-bit lcd_cmd_bits
    // is a different abstraction, so the two aren't directly comparable.
    // Confirmed via github.com/espressif/esp-bsp/issues/724: someone else
    // has esp_lcd_axs15231b + esp_lcd_panel_io_spi genuinely working in
    // QSPI mode (debugging a RASET/Y-position bug, meaning content WAS
    // visible for them) using exactly these vendor-macro values.
    ioCfg.spi_mode = 3;
    ioCfg.pclk_hz = 40 * 1000 * 1000;
    ioCfg.trans_queue_depth = 10;
    ioCfg.lcd_cmd_bits = 32;
    ioCfg.lcd_param_bits = 8;
    ioCfg.flags.quad_mode = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioCfg, &lcdIo));

    axs15231b_vendor_config_t vendorCfg = {};
    vendorCfg.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panelCfg = {};
    panelCfg.reset_gpio_num = -1; // no dedicated reset line on this board (matches GFX_NOT_DEFINED before)
    panelCfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panelCfg.bits_per_pixel = 16;
    panelCfg.vendor_config = &vendorCfg;
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(lcdIo, &panelCfg, &lcdPanel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcdPanel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcdPanel));
    // Rotation intentionally not applied yet — see LS_W/LS_H comment above.
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcdPanel, true));
}

static AXS15231BTouch touch(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_ADDR);

static lv_display_t *lvDisplay;
static lv_indev_t *lvTouchIndev;

// Real backlight brightness control (see AppConfig::brightness / applyConfig()).
#define BL_LEDC_CHANNEL 0
#define BL_LEDC_FREQ_HZ 5000
#define BL_LEDC_RES_BITS 8

static uint32_t flushUs = 0, flushCount = 0;
static uint32_t lastStatsMs = 0;

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t t0 = micros();
    // Real per-area write straight to the panel — no Canvas, no full-frame
    // push. x_end/y_end are exclusive in esp_lcd's draw_bitmap, hence +1.
    esp_lcd_panel_draw_bitmap(lcdPanel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    flushUs += micros() - t0;
    flushCount++;
    lv_display_flush_ready(disp);
}

static void wakeScreen(); // defined with the burn-in helpers below

static void touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    uint16_t x, y;
    if (touch.getPoint(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        wakeScreen();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------------------------------------------------------------------
// AppConfig (spec section 33) — trimmed to fields that have a real,
// observable effect in this sensor-less demo. Persisted to NVS.
// ---------------------------------------------------------------------
struct AppConfig {
    float maxRangeM = 100;
    float minTargetSpeedKmh = 5;
    float maxTargets = 5;

    float audioEnableKmh = 62;
    float hysteresisKmh = 4; // disable threshold = audioEnableKmh - hysteresisKmh
    float ttcWarnS = 3.0f;
    float ttcCritS = 1.5f;
    float minConfidence = 0.70f;

    float brightness = 100; // % — drives the real backlight PWM
    float autoDimMin = 5;   // minutes of no touch before dimming (0 = off)

    bool showId = true;
    bool showSpeed = false;
    bool showTtc = false;
    bool showAngle = false;
    bool audioEnabled = true;
};

static AppConfig cfg;
static Preferences prefs;

static void clampConfig(AppConfig &c) {
    c.maxRangeM = constrain(c.maxRangeM, 10.0f, 100.0f);
    c.minTargetSpeedKmh = constrain(c.minTargetSpeedKmh, 0.0f, 30.0f);
    c.maxTargets = constrain(c.maxTargets, 1.0f, 5.0f);
    c.audioEnableKmh = constrain(c.audioEnableKmh, 40.0f, 120.0f);
    c.hysteresisKmh = constrain(c.hysteresisKmh, 1.0f, 10.0f);
    c.ttcWarnS = constrain(c.ttcWarnS, 1.0f, 10.0f);
    c.ttcCritS = constrain(c.ttcCritS, 0.5f, 5.0f);
    if (c.ttcCritS >= c.ttcWarnS) c.ttcCritS = c.ttcWarnS - 0.1f; // spec section 53 rule
    c.minConfidence = constrain(c.minConfidence, 0.0f, 1.0f);
    c.brightness = constrain(c.brightness, 5.0f, 100.0f);
    c.autoDimMin = constrain(c.autoDimMin, 0.0f, 30.0f);
}

// ---------------------------------------------------------------------
// Screen burn-in / image retention mitigation
// ---------------------------------------------------------------------
// Two independent measures, both required because the Dashboard keeps
// several high-contrast elements in a fixed spot for the whole trip (top
// status bar, bottom TTC/status row, road edge lines):
//   1. Pixel shift — the entire dashboard is nudged a couple of pixels on a
//      slow cycle so no edge sits on the same physical pixels for hours.
//   2. Auto-dim — backlight drops to a low level after a configurable idle
//      period; any touch restores it. Lower luminance dramatically slows
//      image retention on IPS panels.
// NOTE for the real vehicle build: auto-dim must additionally be gated on
// ego speed (never dim while the driver is relying on the warning display) —
// that gate belongs with the safety engine, not here in a bench demo.
static const int kPixelShiftOffsets[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
static int pixelShiftIdx = 0;
static uint32_t lastTouchMs = 0;
static bool screenDimmed = false;

// Arduino-ESP32 3.x replaced the channel-based LEDC API with a pin-based one.
// Both are supported here so this file builds unchanged on env:uidemo (core
// 2.0.17) and env:uidemo3 (core 3.x).
static void backlightBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL, BL_LEDC_FREQ_HZ, BL_LEDC_RES_BITS);
#else
    ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ_HZ, BL_LEDC_RES_BITS);
    ledcAttachPin(TFT_BL, BL_LEDC_CHANNEL);
#endif
}

static void backlightWrite(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL, duty);
#else
    ledcWrite(BL_LEDC_CHANNEL, duty);
#endif
}

static void applyConfig() {
    float level = screenDimmed ? 12.0f : cfg.brightness;
    backlightWrite((uint32_t)(level / 100.0f * 255.0f));
}

static void wakeScreen() {
    lastTouchMs = millis();
    if (screenDimmed) {
        screenDimmed = false;
        applyConfig();
    }
}

static void loadConfigFromNVS() {
    // Read-write (not read-only) so the "radarcar" namespace is created on
    // first boot instead of logging a harmless but noisy NOT_FOUND error.
    prefs.begin("radarcar", false);
    cfg.maxRangeM = prefs.getFloat("maxRange", cfg.maxRangeM);
    cfg.minTargetSpeedKmh = prefs.getFloat("minTgtSpd", cfg.minTargetSpeedKmh);
    cfg.maxTargets = prefs.getFloat("maxTargets", cfg.maxTargets);
    cfg.audioEnableKmh = prefs.getFloat("audioEnKmh", cfg.audioEnableKmh);
    cfg.hysteresisKmh = prefs.getFloat("hyst", cfg.hysteresisKmh);
    cfg.ttcWarnS = prefs.getFloat("ttcWarn", cfg.ttcWarnS);
    cfg.ttcCritS = prefs.getFloat("ttcCrit", cfg.ttcCritS);
    cfg.minConfidence = prefs.getFloat("minConf", cfg.minConfidence);
    cfg.brightness = prefs.getFloat("bright", cfg.brightness);
    cfg.autoDimMin = prefs.getFloat("autoDim", cfg.autoDimMin);
    cfg.showId = prefs.getBool("showId", cfg.showId);
    cfg.showSpeed = prefs.getBool("showSpeed", cfg.showSpeed);
    cfg.showTtc = prefs.getBool("showTtc", cfg.showTtc);
    cfg.showAngle = prefs.getBool("showAngle", cfg.showAngle);
    cfg.audioEnabled = prefs.getBool("audioEn", cfg.audioEnabled);
    prefs.end();
    clampConfig(cfg);
}

static void saveConfigToNVS() {
    prefs.begin("radarcar", false);
    prefs.putFloat("maxRange", cfg.maxRangeM);
    prefs.putFloat("minTgtSpd", cfg.minTargetSpeedKmh);
    prefs.putFloat("maxTargets", cfg.maxTargets);
    prefs.putFloat("audioEnKmh", cfg.audioEnableKmh);
    prefs.putFloat("hyst", cfg.hysteresisKmh);
    prefs.putFloat("ttcWarn", cfg.ttcWarnS);
    prefs.putFloat("ttcCrit", cfg.ttcCritS);
    prefs.putFloat("minConf", cfg.minConfidence);
    prefs.putFloat("bright", cfg.brightness);
    prefs.putFloat("autoDim", cfg.autoDimMin);
    prefs.putBool("showId", cfg.showId);
    prefs.putBool("showSpeed", cfg.showSpeed);
    prefs.putBool("showTtc", cfg.showTtc);
    prefs.putBool("showAngle", cfg.showAngle);
    prefs.putBool("audioEn", cfg.audioEnabled);
    prefs.end();
}

// ---------------------------------------------------------------------
// Simulated data model (spec section 7-13)
// ---------------------------------------------------------------------
#define MAX_TARGETS 5

enum Relation { SAME_LANE, ADJACENT_LEFT, ADJACENT_RIGHT, OPPOSITE, UNKNOWN };

struct SimTarget {
    bool active;
    float distanceM;
    float angleDeg;
    float closingSpeedMps; // positive = approaching
    Relation relation;
    float confidence;
    float ttcS; // computed
};

static SimTarget targets[MAX_TARGETS];
static float egoSpeedKmh = 50;
static bool audioAllowed = false; // with hysteresis, updated each tick
static bool radarOnline = true;
static bool gnssFix = true;
static int primaryIdx = -1;

static lv_color_t riskColor(float ttcS) {
    if (ttcS > 5.0f) return lv_color_hex(0x33CC66);      // GREEN safe
    if (ttcS > cfg.ttcWarnS) return lv_color_hex(0xE0C020); // YELLOW caution
    if (ttcS > cfg.ttcCritS) return lv_color_hex(0xFF8800); // ORANGE warning
    return lv_color_hex(0xFF3333);                          // RED critical
}

static const char *riskLabel(float ttcS) {
    if (ttcS > 5.0f) return "SAFE";
    if (ttcS > cfg.ttcWarnS) return "CAUTION";
    if (ttcS > cfg.ttcCritS) return "WARNING";
    return "CRITICAL";
}

static const char *relationLabel(Relation r) {
    switch (r) {
        case SAME_LANE: return "SAME_LANE";
        case ADJACENT_LEFT: return "ADJ_L";
        case ADJACENT_RIGHT: return "ADJ_R";
        case OPPOSITE: return "OPPOSITE";
        default: return "UNKNOWN";
    }
}

// Per-track synthetic motion so targets glide smoothly instead of jumping
// (spec section 9/28: "khong de target nhay manh giua cac frame").
struct TrackDef {
    Relation relation;
    float distBase, distAmp, distPeriodMs, distPhase;
    float angleDeg;
    float closingBase, closingAmp, closingPeriodMs, closingPhase;
};

static const TrackDef kTracks[MAX_TARGETS] = {
    {SAME_LANE,      35, 25, 9000,  0.0f,   0.0f,  3, 6, 7000, 0.0f},
    {ADJACENT_LEFT,  30, 15, 11000, 1.0f,  -9.0f,  0, 3, 8000, 1.0f},
    {ADJACENT_RIGHT, 40, 18, 12500, 2.0f,   9.0f,  0, 3, 8500, 2.0f},
    {OPPOSITE,       60, 40, 6000,  0.5f,  -3.0f, -18, 6, 6000, 0.5f},
    {UNKNOWN,        50, 20, 15000, 1.5f,   6.0f,  1, 4, 9000, 1.5f},
};

static void updateSimulation(uint32_t nowMs) {
    // Radar / GNSS fail-safe windows (spec T13/T14 — no fake data while "lost")
    radarOnline = fmodf(nowMs, 25000.0f) > 2000.0f;
    gnssFix = fmodf(nowMs + 12000, 35000.0f) > 3000.0f;

    // Ego speed oscillates through the hysteresis band repeatedly.
    if (gnssFix) {
        egoSpeedKmh = 60.0f + 20.0f * sinf((nowMs / 8000.0f) * 2.0f * (float)M_PI);
    }

    // Active target count cycles 0..maxTargets..0 (spec section 29: must
    // show the real count, never a fixed number), capped by cfg.maxTargets.
    int cycleCount = (int)roundf(2.5f + 2.5f * sinf((nowMs / 10000.0f) * 2.0f * (float)M_PI));
    cycleCount = constrain(cycleCount, 0, (int)cfg.maxTargets);

    primaryIdx = -1;
    float bestTtc = 1e9f;

    for (int i = 0; i < MAX_TARGETS; i++) {
        SimTarget &t = targets[i];
        if (!radarOnline || i >= cycleCount) {
            t.active = false;
            continue;
        }
        const TrackDef &tr = kTracks[i];
        float distanceM = tr.distBase + tr.distAmp * sinf((nowMs / tr.distPeriodMs + tr.distPhase) * 2.0f * (float)M_PI);
        if (distanceM < 3.0f) distanceM = 3.0f;
        float closingSpeedMps =
            tr.closingBase + tr.closingAmp * sinf((nowMs / tr.closingPeriodMs + tr.closingPhase) * 2.0f * (float)M_PI);

        // Radar min-target-speed filter (spec section 24) — targets moving
        // slower than the configured threshold are not reported.
        if (fabsf(closingSpeedMps) * 3.6f < cfg.minTargetSpeedKmh) {
            t.active = false;
            continue;
        }

        t.active = true;
        t.distanceM = distanceM;
        t.angleDeg = tr.angleDeg;
        t.closingSpeedMps = closingSpeedMps;
        t.relation = tr.relation;
        t.confidence = 0.85f;
        t.ttcS = (t.closingSpeedMps <= 0.05f) ? INFINITY : (t.distanceM / t.closingSpeedMps);

        if (t.relation == SAME_LANE && t.closingSpeedMps > 0 && t.confidence >= cfg.minConfidence &&
            t.ttcS < bestTtc) {
            bestTtc = t.ttcS;
            primaryIdx = i;
        }
    }

    // Audio gate with hysteresis (spec section 13)
    float audioDisableKmh = cfg.audioEnableKmh - cfg.hysteresisKmh;
    static bool audioSpeedEnabled = false;
    if (egoSpeedKmh > cfg.audioEnableKmh) audioSpeedEnabled = true;
    else if (egoSpeedKmh < audioDisableKmh) audioSpeedEnabled = false;

    audioAllowed = cfg.audioEnabled && gnssFix && audioSpeedEnabled && primaryIdx >= 0 &&
                   targets[primaryIdx].ttcS < cfg.ttcWarnS;
}

// ---------------------------------------------------------------------
// Dashboard screen
// ---------------------------------------------------------------------
static lv_obj_t *dashboardScreen, *settingsScreen;

static lv_obj_t *gpsDot, *gpsLabel, *speedLabel, *radarDot, *radarLabel;
static lv_obj_t *roadArea;
static lv_obj_t *carBody[MAX_TARGETS];
static lv_obj_t *carRoof[MAX_TARGETS];
static lv_obj_t *targetLabels[MAX_TARGETS];
static lv_obj_t *ttcLabel, *targetCountLabel;
static lv_obj_t *radarStatusDot, *gnssStatusDot, *audioStatusDot;
static lv_obj_t *radarStatusLbl, *gnssStatusLbl, *audioStatusLbl;
static lv_obj_t *faultLabel;

// Landscape 480x320 layout
static const int TOP_H = 28, BOTTOM_H = 54;
static const int ROAD_TOP = TOP_H + 4, ROAD_BOTTOM = 320 - BOTTOM_H;
static const int ROAD_LEFT = 20, ROAD_RIGHT = 460;

static void buildRoadLanes() {
    int roadW = ROAD_RIGHT - ROAD_LEFT;
    int centerX = roadW / 2;
    int topHalf = (int)(roadW * 0.16f); // road is narrow near the top (far away)
    int botHalf = roadW / 2;            // and fills the full width at the bottom (near)
    int top = 6, bottom = (ROAD_BOTTOM - ROAD_TOP) - 6;

    auto edgeLine = [&](int topX, int botX) {
        lv_obj_t *line = lv_line_create(roadArea);
        lv_obj_set_style_line_color(line, lv_color_hex(0x3A4A5C), 0);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        static lv_point_precise_t pts[2];
        lv_point_precise_t localPts[2] = {{(lv_value_precise_t)topX, (lv_value_precise_t)top},
                                           {(lv_value_precise_t)botX, (lv_value_precise_t)bottom}};
        memcpy(pts, localPts, sizeof(pts));
        lv_line_set_points(line, pts, 2);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    };
    edgeLine(centerX - topHalf, centerX - botHalf);
    edgeLine(centerX + topHalf, centerX + botHalf);

    // Dashed ego-lane center line
    int dashCount = 6;
    int span = bottom - top;
    for (int i = 0; i < dashCount; i++) {
        int y0 = top + (span * i) / dashCount + 4;
        int y1 = top + (span * (i + 1)) / dashCount - 10;
        if (y1 <= y0) continue;
        lv_obj_t *dash = lv_line_create(roadArea);
        lv_obj_set_style_line_color(dash, lv_color_hex(0x556678), 0);
        lv_obj_set_style_line_width(dash, 2, 0);
        static lv_point_precise_t dashPts[6][2];
        lv_point_precise_t local[2] = {{(lv_value_precise_t)centerX, (lv_value_precise_t)y0},
                                        {(lv_value_precise_t)centerX, (lv_value_precise_t)y1}};
        memcpy(dashPts[i], local, sizeof(local));
        lv_line_set_points(dash, dashPts[i], 2);
        lv_obj_clear_flag(dash, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void onOpenSettings(lv_event_t *) { lv_screen_load(settingsScreen); }

static lv_obj_t *dashRoot; // everything sits in here so it can be pixel-shifted

static void buildDashboard() {
    dashboardScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(dashboardScreen, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_pad_all(dashboardScreen, 0, 0);
    lv_obj_clear_flag(dashboardScreen, LV_OBJ_FLAG_SCROLLABLE);

    // Root container: holds the whole dashboard so the burn-in pixel shift
    // can nudge everything at once, and so a long press anywhere on it opens
    // Settings (spec section 17 suggests long-press for the settings entry).
    dashRoot = lv_obj_create(dashboardScreen);
    lv_obj_set_pos(dashRoot, 0, 0);
    lv_obj_set_size(dashRoot, 480, 320);
    lv_obj_set_style_bg_color(dashRoot, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_border_width(dashRoot, 0, 0);
    lv_obj_set_style_radius(dashRoot, 0, 0);
    lv_obj_set_style_pad_all(dashRoot, 0, 0);
    lv_obj_clear_flag(dashRoot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dashRoot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dashRoot, onOpenSettings, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *scr = dashRoot;

    // --- Top status bar ---
    gpsDot = lv_obj_create(scr);
    lv_obj_set_size(gpsDot, 12, 12);
    lv_obj_set_style_radius(gpsDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(gpsDot, 0, 0);
    lv_obj_set_pos(gpsDot, 12, 8);
    lv_obj_clear_flag(gpsDot, LV_OBJ_FLAG_CLICKABLE);

    gpsLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(gpsLabel, lv_color_hex(0xAAB4C0), 0);
    lv_obj_set_pos(gpsLabel, 30, 5);

    speedLabel = lv_label_create(scr);
    lv_obj_set_style_text_font(speedLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(speedLabel, lv_color_white(), 0);
    lv_obj_align(speedLabel, LV_ALIGN_TOP_MID, 0, 2);

    radarDot = lv_obj_create(scr);
    lv_obj_set_size(radarDot, 12, 12);
    lv_obj_set_style_radius(radarDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(radarDot, 0, 0);
    lv_obj_align(radarDot, LV_ALIGN_TOP_RIGHT, -74, 8);
    lv_obj_clear_flag(radarDot, LV_OBJ_FLAG_CLICKABLE);

    radarLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(radarLabel, lv_color_hex(0xAAB4C0), 0);
    lv_obj_align(radarLabel, LV_ALIGN_TOP_RIGHT, -16, 5);

    // --- Road / target area ---
    roadArea = lv_obj_create(scr);
    lv_obj_set_pos(roadArea, ROAD_LEFT, ROAD_TOP);
    lv_obj_set_size(roadArea, ROAD_RIGHT - ROAD_LEFT, ROAD_BOTTOM - ROAD_TOP);
    lv_obj_set_style_bg_color(roadArea, lv_color_hex(0x12181F), 0);
    lv_obj_set_style_border_color(roadArea, lv_color_hex(0x243040), 0);
    lv_obj_set_style_radius(roadArea, 8, 0);
    lv_obj_clear_flag(roadArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(roadArea, LV_OBJ_FLAG_CLICKABLE);

    buildRoadLanes();

    faultLabel = lv_label_create(roadArea);
    lv_obj_set_style_text_color(faultLabel, lv_color_hex(0xFF5544), 0);
    lv_obj_set_style_text_align(faultLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(faultLabel);
    lv_obj_add_flag(faultLabel, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_TARGETS; i++) {
        carBody[i] = lv_obj_create(roadArea);
        lv_obj_set_style_radius(carBody[i], 3, 0);
        lv_obj_set_style_border_width(carBody[i], 0, 0);
        lv_obj_add_flag(carBody[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(carBody[i], LV_OBJ_FLAG_CLICKABLE);

        carRoof[i] = lv_obj_create(roadArea);
        lv_obj_set_style_radius(carRoof[i], 2, 0);
        lv_obj_set_style_border_width(carRoof[i], 0, 0);
        lv_obj_add_flag(carRoof[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(carRoof[i], LV_OBJ_FLAG_CLICKABLE);

        targetLabels[i] = lv_label_create(roadArea);
        lv_obj_set_style_text_color(targetLabels[i], lv_color_hex(0xCCCCCC), 0);
        lv_obj_add_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // --- Bottom info bar ---
    ttcLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(ttcLabel, lv_color_white(), 0);
    lv_obj_set_pos(ttcLabel, 16, ROAD_BOTTOM + 6);

    targetCountLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(targetCountLabel, lv_color_white(), 0);
    lv_obj_align(targetCountLabel, LV_ALIGN_TOP_RIGHT, -16, ROAD_BOTTOM + 6);

    auto makeStatus = [&](lv_obj_t **dot, lv_obj_t **lbl, int xOff, const char *text) {
        *dot = lv_obj_create(scr);
        lv_obj_set_size(*dot, 10, 10);
        lv_obj_set_style_radius(*dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(*dot, 0, 0);
        lv_obj_set_pos(*dot, xOff, ROAD_BOTTOM + 32);
        lv_obj_clear_flag(*dot, LV_OBJ_FLAG_CLICKABLE);

        *lbl = lv_label_create(scr);
        lv_obj_set_style_text_color(*lbl, lv_color_hex(0xAAB4C0), 0);
        lv_label_set_text(*lbl, text);
        lv_obj_set_pos(*lbl, xOff + 16, ROAD_BOTTOM + 29);
    };
    makeStatus(&radarStatusDot, &radarStatusLbl, 20, "Radar");
    makeStatus(&gnssStatusDot, &gnssStatusLbl, 130, "GNSS");
    makeStatus(&audioStatusDot, &audioStatusLbl, 240, "Audio");

    // Discoverability hint, since there is no visible Settings button anymore.
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5A6675), 0);
    lv_label_set_text(hint, "hold 1s = Settings");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -16, ROAD_BOTTOM + 29);
}

// Named lerpf (not lerp) — the newer toolchain used by env:uidemo3
// (Arduino-ESP32 3.x / a more recent libstdc++) already declares
// `float lerp(float,float,float)` at global scope via <cmath>/C++20,
// and a same-signature redeclaration is a hard conflict there.
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static void refreshDashboard() {
    // Top bar
    lv_obj_set_style_bg_color(gpsDot, gnssFix ? lv_color_hex(0x33CC66) : lv_color_hex(0xFF3333), 0);
    lv_label_set_text(gpsLabel, gnssFix ? "GPS" : "GPS LOST");
    if (gnssFix) {
        // NOTE: LVGL's builtin vsnprintf has %f support compiled out when
        // LV_USE_FLOAT=0 (our lv_conf.h) — passing %f to lv_label_set_text_fmt
        // silently corrupts the varargs and crashes (LoadProhibited).
        // Confirmed on real hardware 2026-09-14. Format floats with the real
        // libc snprintf into a buffer instead, then set the plain string.
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f km/h", egoSpeedKmh);
        lv_label_set_text(speedLabel, buf);
    } else {
        lv_label_set_text(speedLabel, "-- km/h");
    }
    lv_obj_set_style_bg_color(radarDot, radarOnline ? lv_color_hex(0x33CC66) : lv_color_hex(0xFF3333), 0);
    lv_label_set_text(radarLabel, radarOnline ? "RADAR" : "FAULT");

    // Road / targets
    if (!radarOnline) {
        lv_label_set_text(faultLabel, "RADAR FAULT\n(no simulated targets)");
        lv_obj_clear_flag(faultLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(faultLabel, LV_OBJ_FLAG_HIDDEN);
    }

    int roadW = ROAD_RIGHT - ROAD_LEFT;
    int roadH = ROAD_BOTTOM - ROAD_TOP;
    int activeCount = 0;

    for (int i = 0; i < MAX_TARGETS; i++) {
        SimTarget &t = targets[i];
        if (!t.active) {
            lv_obj_add_flag(carBody[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(carRoof[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        activeCount++;

        float lateral = t.distanceM * sinf(t.angleDeg * (float)M_PI / 180.0f);
        float normDist = t.distanceM / cfg.maxRangeM;
        if (normDist > 1) normDist = 1;
        if (normDist < 0) normDist = 0;

        int screenX = roadW / 2 + (int)((lateral / 6.0f) * (roadW / 2));
        if (screenX < 20) screenX = 20;
        if (screenX > roadW - 20) screenX = roadW - 20;
        int screenY = roadH - (int)(normDist * (roadH - 24)) - 12;

        // Car silhouette: wide/tall when near, small when far (perspective).
        int bodyW = (int)lerpf(50, 14, normDist);
        int bodyH = (int)lerpf(28, 9, normDist);
        int roofW = (int)(bodyW * 0.55f);
        int roofH = (int)(bodyH * 0.55f);

        lv_color_t color;
        if (t.relation == OPPOSITE) color = lv_color_hex(0x4488FF);
        else if (t.relation == UNKNOWN) color = lv_color_hex(0x8899AA);
        else color = riskColor(t.ttcS);

        lv_obj_set_size(carBody[i], bodyW, bodyH);
        lv_obj_set_pos(carBody[i], screenX - bodyW / 2, screenY - bodyH / 2);
        lv_obj_set_style_bg_color(carBody[i], color, 0);
        lv_obj_clear_flag(carBody[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(carRoof[i], roofW, roofH);
        lv_obj_set_pos(carRoof[i], screenX - roofW / 2, screenY - bodyH / 2 - roofH / 3);
        lv_obj_set_style_bg_color(carRoof[i], lv_color_darken(color, 110), 0);
        lv_obj_clear_flag(carRoof[i], LV_OBJ_FLAG_HIDDEN);

        // Per-target label content follows the Display settings toggles.
        char buf[48];
        size_t used = 0;
        if (cfg.showId) {
            used += snprintf(buf + used, sizeof(buf) - used, "#%d%s", i + 1, i == primaryIdx ? "P" : "");
        }
        if (cfg.showSpeed) {
            used += snprintf(buf + used, sizeof(buf) - used, "%s%.0fkm/h", used ? " " : "",
                              (double)(t.closingSpeedMps * 3.6f));
        }
        if (cfg.showTtc) {
            if (isinf(t.ttcS)) used += snprintf(buf + used, sizeof(buf) - used, "%sTTC-", used ? " " : "");
            else used += snprintf(buf + used, sizeof(buf) - used, "%sTTC%.1f", used ? " " : "", (double)t.ttcS);
        }
        if (cfg.showAngle) {
            used += snprintf(buf + used, sizeof(buf) - used, "%s%.0fdeg", used ? " " : "", (double)t.angleDeg);
        }
        if (used == 0) buf[0] = '\0';

        if (buf[0]) {
            lv_label_set_text(targetLabels[i], buf);
            lv_obj_set_pos(targetLabels[i], screenX + bodyW / 2 + 3, screenY - 6);
            lv_obj_clear_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(targetLabels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Bottom bar
    lv_label_set_text_fmt(targetCountLabel, "TARGETS: %d", activeCount);

    if (primaryIdx >= 0) {
        SimTarget &p = targets[primaryIdx];
        char buf[48];
        snprintf(buf, sizeof(buf), "TTC %.1fs  %s (%s)", (double)p.ttcS, riskLabel(p.ttcS), relationLabel(p.relation));
        lv_label_set_text(ttcLabel, buf);
        lv_obj_set_style_text_color(ttcLabel, riskColor(p.ttcS), 0);
    } else {
        lv_label_set_text(ttcLabel, "TTC --  SAFE");
        lv_obj_set_style_text_color(ttcLabel, lv_color_hex(0x33CC66), 0);
    }

    lv_obj_set_style_bg_color(radarStatusDot, radarOnline ? lv_color_hex(0x33CC66) : lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_bg_color(gnssStatusDot, gnssFix ? lv_color_hex(0x33CC66) : lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_bg_color(audioStatusDot, audioAllowed ? lv_color_hex(0x33CC66) : lv_color_hex(0x445566), 0);
}

// ---------------------------------------------------------------------
// Settings screen (spec sections 16-17, 55-61)
// ---------------------------------------------------------------------
struct SliderBinding {
    lv_obj_t *slider;
    lv_obj_t *valLabel;
    float *target;
    float divisor;
    const char *unit;
};
static SliderBinding sliderBindings[10];
static int sliderCount = 0;

struct SwitchBinding {
    lv_obj_t *sw;
    bool *target;
};
static SwitchBinding switchBindings[6];
static int switchCount = 0;

static lv_obj_t *settingsStatusLabel;

static void onConfigChanged() {
    clampConfig(cfg);
    applyConfig();
}

static void onSliderChanged(lv_event_t *e) {
    SliderBinding *b = (SliderBinding *)lv_event_get_user_data(e);
    int32_t raw = lv_slider_get_value(b->slider);
    *(b->target) = raw / b->divisor;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f%s", (double)(*(b->target)), b->unit);
    lv_label_set_text(b->valLabel, buf);
    lv_label_set_text(settingsStatusLabel, "");
    onConfigChanged();
}

static void onSwitchChanged(lv_event_t *e) {
    SwitchBinding *b = (SwitchBinding *)lv_event_get_user_data(e);
    *(b->target) = lv_obj_has_state(b->sw, LV_STATE_CHECKED);
    lv_label_set_text(settingsStatusLabel, "");
    onConfigChanged();
}

// Row layout tuned for the ~356px-wide category content panel (see
// buildSettingsScreen). Row height 30px x up to 6 rows = 180px, comfortably
// inside the 260px content height with no scrolling needed.
static void addSliderRow(lv_obj_t *parent, int &y, const char *name, float *target, int32_t minRaw, int32_t maxRaw,
                          float divisor, const char *unit) {
    lv_obj_t *nameLbl = lv_label_create(parent);
    lv_label_set_text(nameLbl, name);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
    lv_obj_set_pos(nameLbl, 4, y + 5);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, 150, 10);
    lv_obj_set_pos(slider, 148, y + 8);
    lv_slider_set_range(slider, minRaw, maxRaw);
    lv_slider_set_value(slider, (int32_t)((*target) * divisor), LV_ANIM_OFF);

    lv_obj_t *valLbl = lv_label_create(parent);
    lv_obj_set_style_text_color(valLbl, lv_color_white(), 0);
    lv_obj_set_pos(valLbl, 306, y + 5);

    SliderBinding *b = &sliderBindings[sliderCount++];
    b->slider = slider;
    b->valLabel = valLbl;
    b->target = target;
    b->divisor = divisor;
    b->unit = unit;
    lv_obj_add_event_cb(slider, onSliderChanged, LV_EVENT_VALUE_CHANGED, b);

    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f%s", (double)(*target), unit);
    lv_label_set_text(valLbl, buf);

    y += 30;
}

static void addSwitchRow(lv_obj_t *parent, int &y, const char *name, bool *target) {
    lv_obj_t *nameLbl = lv_label_create(parent);
    lv_label_set_text(nameLbl, name);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(0xCCD6E0), 0);
    lv_obj_set_pos(nameLbl, 4, y + 3);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, 306, y);
    if (*target) lv_obj_add_state(sw, LV_STATE_CHECKED);

    SwitchBinding *b = &switchBindings[switchCount++];
    b->sw = sw;
    b->target = target;
    lv_obj_add_event_cb(sw, onSwitchChanged, LV_EVENT_VALUE_CHANGED, b);

    y += 30;
}

static void onBackToDashboard(lv_event_t *) { lv_screen_load(dashboardScreen); }

// Save is confirmed through a modal rather than writing straight away: the
// settings being written include the safety thresholds, and NVS writes have
// limited endurance, so an accidental brush of the button should not persist.
static lv_obj_t *confirmOverlay;

static void closeConfirm(lv_event_t *) { lv_obj_add_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN); }

static void onConfirmSaveYes(lv_event_t *) {
    saveConfigToNVS();
    lv_obj_add_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(settingsStatusLabel, "Saved to NVS");
    Serial.println("[uidemo] config saved to NVS");
}

static void onSaveConfig(lv_event_t *) { lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN); }

static void buildConfirmOverlay(lv_obj_t *parent) {
    confirmOverlay = lv_obj_create(parent);
    lv_obj_set_pos(confirmOverlay, 0, 0);
    lv_obj_set_size(confirmOverlay, 480, 320);
    lv_obj_set_style_bg_color(confirmOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirmOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(confirmOverlay, 0, 0);
    lv_obj_set_style_radius(confirmOverlay, 0, 0);
    lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirmOverlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(confirmOverlay);
    lv_obj_set_size(box, 300, 130);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1B222A), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x3A4A5C), 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, "Save settings to memory?");
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *noBtn = lv_button_create(box);
    lv_obj_set_size(noBtn, 110, 36);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_bg_color(noBtn, lv_color_hex(0x44505C), 0);
    lv_obj_add_event_cb(noBtn, closeConfirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *noLbl = lv_label_create(noBtn);
    lv_label_set_text(noLbl, "Cancel");
    lv_obj_center(noLbl);

    lv_obj_t *yesBtn = lv_button_create(box);
    lv_obj_set_size(yesBtn, 110, 36);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(yesBtn, lv_color_hex(0x2E7D4F), 0);
    lv_obj_add_event_cb(yesBtn, onConfirmSaveYes, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yesLbl = lv_label_create(yesBtn);
    lv_label_set_text(yesLbl, "Save");
    lv_obj_center(yesLbl);
}

static void onRestoreDefaults(lv_event_t *) {
    cfg = AppConfig();
    clampConfig(cfg);
    applyConfig();
    for (int i = 0; i < sliderCount; i++) {
        SliderBinding &b = sliderBindings[i];
        lv_slider_set_value(b.slider, (int32_t)((*b.target) * b.divisor), LV_ANIM_OFF);
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1f%s", (double)(*b.target), b.unit);
        lv_label_set_text(b.valLabel, buf);
    }
    for (int i = 0; i < switchCount; i++) {
        SwitchBinding &b = switchBindings[i];
        if (*b.target) lv_obj_add_state(b.sw, LV_STATE_CHECKED);
        else lv_obj_clear_state(b.sw, LV_STATE_CHECKED);
    }
    lv_label_set_text(settingsStatusLabel, "Restored defaults");
    Serial.println("[uidemo] config restored to defaults (not yet saved)");
}

// Category menu: left nav rail + one content panel per category, all
// pre-built and toggled via LV_OBJ_FLAG_HIDDEN (no rebuild/flicker on
// switching). Whole screen fits with no scrolling; Save/Defaults stay in a
// fixed footer visible from every category.
static lv_obj_t *categoryPanels[3];
static lv_obj_t *navButtons[3];

static void selectCategory(int idx) {
    for (int i = 0; i < 3; i++) {
        if (i == idx) {
            lv_obj_clear_flag(categoryPanels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(navButtons[i], lv_color_hex(0x2E4A66), 0);
        } else {
            lv_obj_add_flag(categoryPanels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(navButtons[i], lv_color_hex(0x151C24), 0);
        }
    }
}
static void onNavCategory(lv_event_t *e) { selectCategory((int)(intptr_t)lv_event_get_user_data(e)); }

static void buildSettingsScreen() {
    settingsScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settingsScreen, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_pad_all(settingsScreen, 0, 0);
    lv_obj_clear_flag(settingsScreen, LV_OBJ_FLAG_SCROLLABLE);

    const int HEADER_H = 26, FOOTER_H = 34;
    const int NAV_W = 96;
    const int BODY_TOP = HEADER_H, BODY_H = 320 - HEADER_H - FOOTER_H;

    // Header
    lv_obj_t *header = lv_obj_create(settingsScreen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 480, HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x151C24), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *backBtn = lv_button_create(header);
    lv_obj_set_size(backBtn, 64, 20);
    lv_obj_align(backBtn, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(backBtn, onBackToDashboard, LV_EVENT_CLICKED, NULL);
    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, "< Back");
    lv_obj_center(backLbl);

    // Left nav rail
    lv_obj_t *navRail = lv_obj_create(settingsScreen);
    lv_obj_set_pos(navRail, 0, BODY_TOP);
    lv_obj_set_size(navRail, NAV_W, BODY_H);
    lv_obj_set_style_bg_color(navRail, lv_color_hex(0x0E141B), 0);
    lv_obj_set_style_border_width(navRail, 0, 0);
    lv_obj_set_style_radius(navRail, 0, 0);
    lv_obj_set_style_pad_all(navRail, 4, 0);
    lv_obj_clear_flag(navRail, LV_OBJ_FLAG_SCROLLABLE);

    static const char *kCategoryNames[3] = {"Radar", "Safety", "Display"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(navRail);
        lv_obj_set_size(btn, NAV_W - 8, 46);
        lv_obj_set_pos(btn, 0, i * 52);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, onNavCategory, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, kCategoryNames[i]);
        lv_obj_center(lbl);
        navButtons[i] = btn;
    }

    // Content panels (one per category, same rect, toggled via HIDDEN)
    for (int i = 0; i < 3; i++) {
        lv_obj_t *panel = lv_obj_create(settingsScreen);
        lv_obj_set_pos(panel, NAV_W + 4, BODY_TOP);
        lv_obj_set_size(panel, 480 - NAV_W - 4, BODY_H);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B0F14), 0);
        lv_obj_set_style_border_width(panel, 0, 0);
        lv_obj_set_style_pad_all(panel, 4, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        categoryPanels[i] = panel;
    }

    int y;
    y = 4;
    addSliderRow(categoryPanels[0], y, "Max range", &cfg.maxRangeM, 10, 100, 1.0f, " m");
    addSliderRow(categoryPanels[0], y, "Min target speed", &cfg.minTargetSpeedKmh, 0, 30, 1.0f, " km/h");
    addSliderRow(categoryPanels[0], y, "Max targets", &cfg.maxTargets, 1, 5, 1.0f, "");

    y = 4;
    addSliderRow(categoryPanels[1], y, "Audio enable speed", &cfg.audioEnableKmh, 40, 120, 1.0f, " km/h");
    addSliderRow(categoryPanels[1], y, "Hysteresis", &cfg.hysteresisKmh, 1, 10, 1.0f, " km/h");
    addSliderRow(categoryPanels[1], y, "TTC warning", &cfg.ttcWarnS, 10, 100, 10.0f, " s");
    addSliderRow(categoryPanels[1], y, "TTC critical", &cfg.ttcCritS, 5, 50, 10.0f, " s");
    addSliderRow(categoryPanels[1], y, "Min confidence", &cfg.minConfidence, 0, 100, 100.0f, "");
    addSwitchRow(categoryPanels[1], y, "Audio enabled", &cfg.audioEnabled);

    y = 4;
    addSliderRow(categoryPanels[2], y, "Brightness", &cfg.brightness, 5, 100, 1.0f, " %");
    addSliderRow(categoryPanels[2], y, "Auto-dim after", &cfg.autoDimMin, 0, 30, 1.0f, " min");
    addSwitchRow(categoryPanels[2], y, "Show target ID", &cfg.showId);
    addSwitchRow(categoryPanels[2], y, "Show target speed", &cfg.showSpeed);
    addSwitchRow(categoryPanels[2], y, "Show target TTC", &cfg.showTtc);
    addSwitchRow(categoryPanels[2], y, "Show target angle", &cfg.showAngle);

    selectCategory(0);

    // Footer — always visible regardless of selected category
    lv_obj_t *footer = lv_obj_create(settingsScreen);
    lv_obj_set_pos(footer, 0, 320 - FOOTER_H);
    lv_obj_set_size(footer, 480, FOOTER_H);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x151C24), 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    settingsStatusLabel = lv_label_create(footer);
    lv_obj_set_style_text_color(settingsStatusLabel, lv_color_hex(0x66CC88), 0);
    lv_label_set_text(settingsStatusLabel, "");
    lv_obj_align(settingsStatusLabel, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *restoreBtn = lv_button_create(footer);
    lv_obj_set_size(restoreBtn, 90, 24);
    lv_obj_align(restoreBtn, LV_ALIGN_RIGHT_MID, -108, 0);
    lv_obj_add_event_cb(restoreBtn, onRestoreDefaults, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restoreLbl = lv_label_create(restoreBtn);
    lv_label_set_text(restoreLbl, "Defaults");
    lv_obj_center(restoreLbl);

    lv_obj_t *saveBtn = lv_button_create(footer);
    lv_obj_set_size(saveBtn, 90, 24);
    lv_obj_align(saveBtn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(saveBtn, lv_color_hex(0x2E7D4F), 0);
    lv_obj_add_event_cb(saveBtn, onSaveConfig, LV_EVENT_CLICKED, NULL);
    lv_obj_t *saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, "Save");
    lv_obj_center(saveLbl);

    buildConfirmOverlay(settingsScreen); // created last so it covers everything
}

static void simTimerCb(lv_timer_t *) {
    updateSimulation(millis());
    refreshDashboard();
}

// Runs once a minute: nudges the whole dashboard by a couple of pixels so no
// static edge burns in, and applies the idle auto-dim.
static void burnInTimerCb(lv_timer_t *) {
    pixelShiftIdx = (pixelShiftIdx + 1) % 4;
    lv_obj_set_pos(dashRoot, kPixelShiftOffsets[pixelShiftIdx][0], kPixelShiftOffsets[pixelShiftIdx][1]);

    if (cfg.autoDimMin > 0 && !screenDimmed) {
        uint32_t idleMs = millis() - lastTouchMs;
        if (idleMs > (uint32_t)(cfg.autoDimMin * 60000.0f)) {
            screenDimmed = true;
            applyConfig();
            Serial.println("[uidemo] idle -> screen dimmed (burn-in mitigation)");
        }
    }
}

// ---------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[uidemo] UI verification demo (simulated data, no real sensors)");

    loadConfigFromNVS();

    backlightBegin();
    applyConfig(); // sets initial backlight duty from loaded cfg.brightness

    initDisplay();
    Serial.printf("[uidemo] esp_lcd display ready: %dx%d\n", LS_W, LS_H);

    // RAW TEST — bypass LVGL entirely. Fills the whole screen red via
    // repeated direct esp_lcd_panel_draw_bitmap calls (small DMA-safe
    // buffer, same size class as the real LVGL buffers — a single
    // screen-sized buffer failed to allocate from DMA-capable RAM, which
    // is a separate, expected constraint, not a display bug). If this
    // doesn't show red, the problem is below LVGL (bus/panel/init), not in
    // the LVGL buffer/flush wiring.
    {
        const int bandRows = 20;
        size_t rawBufPixels = (size_t)LS_W * bandRows;
        uint16_t *rawBuf = (uint16_t *)heap_caps_malloc(rawBufPixels * 2, MALLOC_CAP_DMA);
        Serial.printf("[uidemo] rawtest buffer alloc: %s\n", rawBuf ? "OK" : "FAILED");
        if (rawBuf) {
            for (size_t i = 0; i < rawBufPixels; i++) rawBuf[i] = 0xF800; // RGB565 red
            esp_err_t lastErr = ESP_OK;
            for (int y = 0; y < LS_H; y += bandRows) {
                int h = min(bandRows, LS_H - y);
                lastErr = esp_lcd_panel_draw_bitmap(lcdPanel, 0, y, LS_W, y + h, rawBuf);
                if (lastErr != ESP_OK) break;
            }
            Serial.printf("[uidemo] rawtest draw_bitmap result: %s\n", esp_err_to_name(lastErr));
        }
    }

    if (!touch.begin()) Serial.println("[uidemo] ERROR: touch I2C init failed");
    touch.setRotation(0); // matches native portrait diagnostic above
    touch.enableOffsetCorrection(true);
    touch.setOffsets(TOUCH_X_MIN, TOUCH_X_MAX, TFT_RES_W - 1, TOUCH_Y_MIN, TOUCH_Y_MAX, TFT_RES_H - 1);

    lv_init();
    lvDisplay = lv_display_create(LS_W, LS_H);
    lv_display_set_flush_cb(lvDisplay, disp_flush_cb);

    // DIAGNOSTIC: MALLOC_CAP_DMA, not just MALLOC_CAP_INTERNAL. The SPI/DMA
    // engine underneath esp_lcd_panel_draw_bitmap needs DMA-capable memory;
    // internal RAM without MALLOC_CAP_DMA isn't guaranteed to qualify, and
    // PSRAM needs the io_config psram_dma_direct flag (not set). Either
    // gap would let a transaction "succeed" at the driver level while the
    // DMA engine reads from memory it can't actually access — silent no-op,
    // matching the clean-black-screen symptom seen so far.
    size_t bufBytes = (size_t)LS_W * 20 * sizeof(lv_color_t);
    lv_color_t *drawBuf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
    lv_color_t *drawBuf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
    if (!drawBuf1 || !drawBuf2) {
        Serial.println("[uidemo] ERROR: MALLOC_CAP_DMA draw buffer allocation failed");
    }
    lv_display_set_buffers(lvDisplay, drawBuf1, drawBuf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvTouchIndev = lv_indev_create();
    lv_indev_set_type(lvTouchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchIndev, touch_read_cb);
    lv_timer_set_period(lv_indev_get_read_timer(lvTouchIndev), 10); // default ~30ms felt laggy on drag; poll faster
    lv_indev_set_long_press_time(lvTouchIndev, 1000);               // hold 1s anywhere on the dashboard = Settings

    buildDashboard();
    buildSettingsScreen();
    lv_screen_load(dashboardScreen);

    lastTouchMs = millis();
    updateSimulation(0);
    refreshDashboard();
    lv_timer_create(simTimerCb, 150, NULL);
    lv_timer_create(burnInTimerCb, 60000, NULL);

    Serial.println("[uidemo] running — hold the screen 1s to open Settings");
}

void loop() {
    static uint32_t lastTick = millis();
    uint32_t now = millis();
    lv_tick_inc(now - lastTick);
    lastTick = now;
    lv_timer_handler();

    // Report where the frame time actually goes, so tuning stops being guesswork.
    // "flush" here is now a real per-dirty-area esp_lcd write, not a whole-frame
    // push — count and avg-time both directly reflect the actual UI update rate.
    if (now - lastStatsMs > 5000) {
        lastStatsMs = now;
        if (flushCount) {
            Serial.printf("[perf] area-writes=%lu  avg=%luus\n", flushCount, flushUs / flushCount);
        }
        flushUs = flushCount = 0;
    }
    delay(1);
}
