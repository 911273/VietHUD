// VietHUD (formerly radar_car) — Phase 1 board bring-up demo (LCD + Touch + LVGL)
//
// SUPERSEDED as the product build 2026-09-14 by src/main_viethud.cpp (the
// real Dashboard/Settings/GNSS app, built by env:viethud — now
// platformio.ini's default_envs; renamed from main_ui_demo.cpp/env:uidemo
// 2026-09-21 when radar was removed entirely). This file is kept in the
// tree and still builds under env:jc3248w535, same role as env:rawtest: a
// minimal, known-good LCD+touch-only build for isolating display/touch
// hardware issues from application logic, not something to extend with
// product features. If you're looking for the actual app, see
// main_viethud.cpp instead.
//
// What this proves out on its own:
//   1. PlatformIO build + flash for the JC3248W535 (ESP32-S3-N16R8V) board
//   2. USB serial log
//   3. LCD init (Arduino_GFX + AXS15231B QSPI panel)
//   4. Touch init (AXS15231B combo touch controller, I2C)
//   5. A basic LVGL screen, with live diagnostics + a touch echo widget
//
// Pin mapping: include/pincfg.h (confirmed against JC3248W535 for this
// exact board — see docs/V1.2_hardening_proposal.md, "Ghi chú board
// bring-up").

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

#include "pincfg.h"
#include "dispcfg.h"
#include "AXS15231BTouch.h"

// ---------------------------------------------------------------------
// Display + touch objects
// ---------------------------------------------------------------------
//
// IMPORTANT: this panel driver's partial-region addressing is unreliable —
// confirmed on real hardware 2026-09-14: a full-screen fillScreen() renders
// correctly, but draw16bitRGBBitmap() at an arbitrary (x,y) sub-region draws
// to the wrong address (overlapping garbage near the top of the screen,
// nothing where it should be). We work around it the way the confirmed
// community reference does: draw into an Arduino_Canvas RAM framebuffer
// (plain array writes, no hardware addressing involved) and only ever push
// pixels to the physical panel via one full-frame canvas->flush() call.
static Arduino_DataBus *bus = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
static Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED /*RST*/, 0, false, TFT_RES_W, TFT_RES_H);
static Arduino_Canvas *gfx = new Arduino_Canvas(TFT_RES_W, TFT_RES_H, panel, 0, 0, 0);

static AXS15231BTouch touch(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_ADDR);

// ---------------------------------------------------------------------
// LVGL glue
// ---------------------------------------------------------------------
static lv_display_t *lvDisplay;
static lv_indev_t *lvTouchIndev;
static lv_obj_t *touchEchoLabel;
static lv_obj_t *touchDot;
static lv_obj_t *uptimeLabel;
static lv_obj_t *heapLabel;
static lv_obj_t *psramLabel;

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    // Writes into the Canvas's RAM framebuffer only — safe regardless of
    // (x,y), no hardware addressing involved yet.
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    if (lv_display_flush_is_last(disp)) {
        // Single full-frame push to the physical panel (the one operation
        // confirmed to address correctly on this board).
        gfx->flush();
    }
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    if (touch.getPoint(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;

        lv_label_set_text_fmt(touchEchoLabel, "Touch: x=%d y=%d", x, y);
        lv_obj_clear_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(touchDot, x - lv_obj_get_width(touchDot) / 2, y - lv_obj_get_height(touchDot) / 2);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------------------------------------------------------------------
// Demo screen
// ---------------------------------------------------------------------
static void buildDemoScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "VietHUD - Phase 1 Bring-up");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8899AA), 0);
    lv_label_set_text(subtitle, "ESP32-S3 / JC3248W535 - LCD + Touch + LVGL");
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 44);

    // Diagnostics panel
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 260, 110);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 12, 76);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1B222A), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x334455), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);

    lv_obj_t *panelTitle = lv_label_create(panel);
    lv_obj_set_style_text_color(panelTitle, lv_color_hex(0x66CCFF), 0);
    lv_label_set_text(panelTitle, "Diagnostics");

    uptimeLabel = lv_label_create(panel);
    lv_obj_set_style_text_color(uptimeLabel, lv_color_white(), 0);

    heapLabel = lv_label_create(panel);
    lv_obj_set_style_text_color(heapLabel, lv_color_white(), 0);

    psramLabel = lv_label_create(panel);
    lv_obj_set_style_text_color(psramLabel, lv_color_white(), 0);

    // Touch echo
    touchEchoLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(touchEchoLabel, lv_color_hex(0xFFCC66), 0);
    lv_label_set_text(touchEchoLabel, "Touch: (chua co du lieu)");
    lv_obj_align(touchEchoLabel, LV_ALIGN_BOTTOM_MID, 0, -16);

    touchDot = lv_obj_create(scr);
    lv_obj_set_size(touchDot, 24, 24);
    lv_obj_set_style_radius(touchDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touchDot, lv_color_hex(0xFF5544), 0);
    lv_obj_set_style_border_width(touchDot, 0, 0);
    lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(touchDot, LV_OBJ_FLAG_CLICKABLE);
}

static void diagnosticsTimerCb(lv_timer_t *) {
    lv_label_set_text_fmt(uptimeLabel, "Uptime: %lu s", millis() / 1000);
    lv_label_set_text_fmt(heapLabel, "Free heap: %u KB", (unsigned)(ESP.getFreeHeap() / 1024));
    lv_label_set_text_fmt(psramLabel, "Free PSRAM: %u KB", (unsigned)(ESP.getFreePsram() / 1024));
}

// ---------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[viethud-bringup] Phase 1 bring-up demo starting...");
    Serial.printf("[viethud-bringup] Chip: %s rev %d, %d MHz, %d cores\n", ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getCpuFreqMHz(), ESP.getChipCores());
    Serial.printf("[viethud-bringup] Flash: %u MB, PSRAM: %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                  (unsigned)(ESP.getPsramSize() / (1024 * 1024)));

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    if (!gfx->begin()) {
        Serial.println("[viethud-bringup] ERROR: display init failed");
    }
    // NOTE: do NOT call gfx->setRotation() on the raw panel driver — on this
    // board it corrupts the write-address window and the screen stays blank
    // (confirmed on real hardware). TFT_ROTATION is 0 (native portrait) in
    // dispcfg.h specifically to avoid ever exercising that code path.
    gfx->fillScreen(BLACK);
    Serial.printf("[viethud-bringup] Display ready: %dx%d\n", gfx->width(), gfx->height());

    if (!touch.begin()) {
        Serial.println("[viethud-bringup] ERROR: touch I2C init failed");
    }
    touch.setRotation(TFT_ROTATION);
    touch.enableOffsetCorrection(true);
    touch.setOffsets(TOUCH_X_MIN, TOUCH_X_MAX, TFT_RES_W - 1, TOUCH_Y_MIN, TOUCH_Y_MAX, TFT_RES_H - 1);

    lv_init();

    lvDisplay = lv_display_create(gfx->width(), gfx->height());
    lv_display_set_flush_cb(lvDisplay, disp_flush_cb);

    size_t bufPixels = (size_t)gfx->width() * 40; // ~40 rows per partial buffer
    size_t bufBytes = bufPixels * sizeof(lv_color_t);
    lv_color_t *drawBuf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
    lv_color_t *drawBuf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(lvDisplay, drawBuf1, drawBuf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvTouchIndev = lv_indev_create();
    lv_indev_set_type(lvTouchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchIndev, touch_read_cb);

    buildDemoScreen();
    lv_timer_create(diagnosticsTimerCb, 500, NULL);

    Serial.println("[viethud-bringup] Phase 1 bring-up complete. LVGL demo running.");
}

void loop() {
    static uint32_t lastTick = millis();
    uint32_t now = millis();
    lv_tick_inc(now - lastTick);
    lastTick = now;

    lv_timer_handler();
    delay(5);
}
