#pragma once
// VietHUD Lite 1.54" screen + buttons: the stand-alone Lite firmware's own
// HUD (big speed-limit sign, speed, alert card, road name, battery, settings
// menu, Wi-Fi/OTA page) drawn straight with Arduino_GFX — no LVGL, no map —
// on top of the platform's GNSS, map matching, alert sounds, Wi-Fi and OTA.
// main_viethud.cpp uses it instead of the LVGL UI when BOARD_UI_LITE.
void liteUiBegin(); // in setup(), after loadConfigFromNVS()/audioInit()
void liteUiTick();  // every loop()

// Full-screen status page for main_viethud.cpp's data-update mode and the
// install-result notice (drawn with the Lite's own OTA screens).
#include <stdint.h>
void liteStatusOpen(const char *title, uint32_t accent);
void liteStatusIcon(bool ok, uint32_t color);
void liteStatusMsg(const char *text);
void liteStatusBar(int pct);
void liteStatusSub(const char *text);
void liteStatusHint(const char *text);
void liteStatusPump(); // redraw (rate-limited)
