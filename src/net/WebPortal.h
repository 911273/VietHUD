#pragma once
#include <stddef.h> // size_t

// Local web portal over the ESP32-S3's built-in WiFi (Access Point mode —
// the device creates its own hotspot, no dependency on any external
// network, since this rides in a vehicle). Three things, all served from
// one small built-in WebServer instance on its own FreeRTOS task (Core 0,
// same pattern as gnss/touch — see gnss/GNSS.h):
//
//   1. Live telemetry ("/") — a browser page polling /api/status (plain
//      JSON, hand-rolled with snprintf, no ArduinoJson dependency) every
//      500ms for the same GNSS/speed-map/heap numbers the serial [gnss]/
//      [sdmgr]/[mem] debug lines already carry. Lets someone watch the link
//      health (fix/sats/speed, speed-map status) from a phone during a real
//      drive without a USB cable tethered to a laptop the whole time, which
//      every debugging session up to now has required.
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

// STA (internet/station) connection info line, decoded for display on the
// on-screen Settings WiFi tab and the web. Shows the joined network + IP + RSSI
// + NTP when connected, or a plain-language reason when not (incl. the iPhone
// 5 GHz-hotspot case). Added 2026-09-25.
void webPortalStaInfo(char *buf, size_t cap);

// The device's AP SSID (custom or auto "VietHUD-XXXX"), computed even when the AP
// is off — for the on-screen QR "join my hotspot" setup code (Settings.cpp).
void webPortalApSsid(char *buf, size_t cap);

// The device's station (STA) IP once joined to a WiFi — for the on-screen "IP
// after connect" display. false + empty buf when not connected.
bool webPortalStaIp(char *buf, size_t cap);

// --- WiFi scan + STA reconnect (2026-09-25), for the on-screen "WiFi setup"
// overlay. All WiFi.* calls stay on the web task, so the UI (Core 1) only sets
// requests / reads cached results — never touches the radio directly. ---
void webPortalStartScan();          // request an async scan of nearby 2.4GHz APs
int  webPortalScanState();          // -1 idle, -2 scanning, >=0 = number of results ready
// Reads result i (0..state-1): fills ssid, rssi (dBm), locked (needs password). Returns 1 if valid.
int  webPortalScanResult(int i, char *ssid, size_t cap, int *rssi, bool *locked);
// Apply new STA creds (already written into cfg by the caller): reconnect the
// station with them (turns WiFi on if it was off). Runs on the web task.
void webPortalReconnectSta();

// WiFi Manager (2026-09-26): manage the list of remembered station networks.
// All mutate AppConfig + persist to NVS and kick a reconnect; safe to call from
// the on-screen UI (Core 1) or the web handlers (web task) — plain cfg writes,
// same pattern as the existing /config POST.
int  webPortalSavedCount();                                  // # of saved networks
bool webPortalSavedNetwork(int i, char *ssid, size_t cap);   // SSID of saved network i
bool webPortalAddNetwork(const char *ssid, const char *password);  // add/update by SSID
bool webPortalDeleteNetwork(int idx);                        // remove saved network idx

// NTP-synced local time (feature F, 2026-09-25). Returns true and fills
// hour/minute (local, VN UTC+7) once the device has joined a station network
// (AppConfig staSsid) and NTP has delivered a plausible time — lets the
// Dashboard clock show the right time without waiting for a GPS fix. Returns
// false when not yet synced, so the caller keeps using GPS time as before.
bool webPortalLocalTime(int *hour, int *minute);
