#pragma once
#include <stddef.h> // size_t

// Local web portal over the ESP32-S3's built-in WiFi (Access Point mode —
// the device creates its own hotspot, no dependency on any external
// network, since this rides in a vehicle). Three things, all served from
// one small built-in WebServer instance on its own FreeRTOS task (Core 0,
// same pattern as radar/GNSS/touch — see radar/LD2451.h):
//
//   1. Live telemetry ("/") — a browser page polling /api/status (plain
//      JSON, hand-rolled with snprintf, no ArduinoJson dependency) every
//      500ms for the same radar/GNSS/heap numbers the serial [radar]/
//      [gnss]/[mem] debug lines already carry. Lets someone watch the link
//      health (frames/parseErrors/snr, fix/sats/speed) from a phone during
//      a real drive without a USB cable tethered to a laptop the whole
//      time, which every debugging session up to now has required.
//   2. Remote config ("/config") — an HTML form over the same AppConfig
//      fields Settings.cpp's sliders bind, for typing exact values instead
//      of dragging a small touchscreen slider. Submitting it clamps +
//      sanitizes + saves to NVS immediately (a web form submit is already a
//      deliberate action, unlike a touchscreen slider that can be brushed
//      by accident — see Settings.cpp's confirm-modal comment for why THAT
//      one needs a confirm step and this one doesn't).
//   3. OTA update ("/update") — standard ESP32-Arduino HTTP-upload OTA
//      (Update.h, built into the core, no extra library) so a new .bin can
//      be pushed over WiFi instead of re-opening the case for a USB flash
//      every build/flash/verify cycle.
//
// Deliberately built on the synchronous WiFi.h/WebServer.h/Update.h trio
// that ships with arduino-esp32 (no AsyncTCP/ESPAsyncWebServer dependency):
// this project's lib_deps list has stayed intentionally short (GFX, lvgl,
// TinyGPSPlus, Preferences), and 500ms-cadence polling doesn't need
// WebSockets' real-time push — plain HTTP polling is simpler, has fewer
// moving parts to get wrong, and needs nothing fetched beyond the core
// that's already required for WiFi at all.
//
// WiFi itself defaults OFF (user-requested 2026-09-14: "Mặc định là wifi
// tắt") — webPortalInit() only starts the background task and registers
// routes; it does NOT bring the AP/radio up. Two ways to turn it on, both
// funneling through webPortalRequestEnable() so WiFi.*/server.*() calls
// only ever happen from the web task's own context (see WebPortal.cpp):
//   - Dashboard: hold anywhere on screen for 3s+ (past the 1s Settings-open
//     point — see Dashboard.cpp's onDashLongPressedRepeat()). Same gesture
//     toggles either direction: off->on or on->off.
//   - Settings > WiFi: an explicit switch, for when you'd rather not guess
//     at hold timing.
// Deliberately not persisted across reboots — only the SSID/password
// (AppConfig's wifiSsid/wifiPassword) survive a reboot, never the on/off
// state, so the device never comes up broadcasting an AP unless someone at
// the wheel/screen actually asks for it this session.
void webPortalInit();

// Requests the web portal turn WiFi on/off. Safe to call from any task
// (Dashboard's touch handling runs on Core 1, the web task itself on Core
// 0) — this only sets a flag; the web task applies it from its own context
// on its next loop iteration (see WebPortal.cpp's applyWifiState()), same
// reasoning AppConfig.h gives for its own cross-task reads being safe
// without a lock: a single bool has no torn/garbage intermediate state.
void webPortalRequestEnable(bool on);

// Current ACTUAL state (what the web task has applied, not merely
// requested) — for the Dashboard/Settings to display without guessing.
bool webPortalIsEnabled();

// Formats a short status string ("OFF" or "ON, IP=192.168.4.1") into buf —
// used by Settings.cpp's WiFi tab. A formatted buffer, not a getter
// returning the AP's IPAddress/String directly, so callers outside this
// file don't need a WiFi.h dependency just to display status text (same
// "callers get a plain data copy, not the live object" reasoning
// core/SharedState.h's snapshot structs are built on).
void webPortalStatusText(char *buf, size_t cap);
