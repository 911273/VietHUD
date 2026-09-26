#include "WebPortal.h"
#include "core/AppConfig.h"
#include "core/NvsStore.h"
#include "core/SharedState.h"
#include "map/SdCardManager.h" // sdMgrListTripLogs()/sdMgrReadFileChunk() — /triplog download page
#include "map/SpeedLimitManager.h" // speedLimitManagerGetInfo()/speedSourceStr() — /api/speedlimit, /api/speedmap/debug
#include "ui/Dashboard.h" // applyConfig() — a web-submitted brightness change needs the same PWM rewrite Settings.cpp's slider does
#include "audio/AudioPlayer.h" // audioSelfTest() — web control panel "test audio" action
#include "demo/DemoMode.h"      // demoModeSetEnabled()/IsEnabled() — web control panel demo toggle
#include "net/DataUpdater.h"    // dataUpdateStart()/GetStatus() — online data update action
#include "net/UpdateApi.h"      // Phone Update Bridge API (/api/v1/*)
#include "net/PortalPage.h"     // kPortalHtml — the "/" single-page portal
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>  // MDNS — viethud.local (feature B)
#include <DNSServer.h> // captive portal (feature E)
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>   // configTime()/time()/localtime() — NTP time sync over STA (feature F)
#include <math.h> // isinf() — used directly below, not just transitively via AppConfig.h
#include <string.h> // strlen() — used in applyWifiState()'s password-length check

// AP mode, not STA: this rides in a vehicle, so "join the existing network"
// isn't a stable concept the way it is for a fixed installation — the
// device brings its own hotspot instead, same reasoning as GNSS being wired
// directly rather than depending on anything external. SSID/password
// now live in AppConfig (cfg.wifiSsid/cfg.wifiPassword — editable from
// Settings > WiFi or /config), not hardcoded here.
static PortalServer server(80); // WebServer + raw-upload read-timeout hook (net/UpdateApi.h)
static DNSServer dnsServer;                 // captive portal: answers every lookup with the AP IP (feature E)
static const char *kMdnsHost = "viethud";   // -> http://viethud.local (feature B)
static const IPAddress kApIp(192, 168, 4, 1); // default SoftAP address, used by the captive portal

// Station/NTP state (feature F). staConnected tracks the last-seen STA link so a
// connect/disconnect only logs + (re)syncs NTP once. ntpSynced flips true after
// the first time NTP delivers a plausible epoch (> 2021), after which the clock
// can use it as a GPS-independent time source.
static volatile bool staConnected = false;
static volatile bool ntpSynced = false;
static char g_apSsid[40] = "VietHUD"; // the actual AP SSID in use (set in applyWifiState) — for status display

// WiFi scan cache (filled by the web task, read by the UI). g_scanCount: -1 idle,
// -2 scanning, >=0 results ready. g_scanReq triggers a fresh async scan.
#define SCAN_MAX 16
static char g_scanSsid[SCAN_MAX][33];
static int8_t g_scanRssi[SCAN_MAX];
static uint8_t g_scanLocked[SCAN_MAX];
static volatile int g_scanCount = -1;
static volatile bool g_scanReq = false;
static volatile bool g_staReconnectReq = false;

void webPortalStartScan() { g_scanReq = true; g_scanCount = -2; }
int webPortalScanState() { return g_scanCount; }
int webPortalScanResult(int i, char *ssid, size_t cap, int *rssi, bool *locked) {
    if (i < 0 || i >= g_scanCount) return 0;
    strncpy(ssid, g_scanSsid[i], cap - 1);
    ssid[cap - 1] = '\0';
    if (rssi) *rssi = g_scanRssi[i];
    if (locked) *locked = g_scanLocked[i] != 0;
    return 1;
}
void webPortalReconnectSta() { g_staReconnectReq = true; }

// ---- WiFi Manager: saved-network list (multiple remembered STA networks) ----
// Runtime candidate state (web task only): the saved-network indices found in
// the last scan, ordered best-RSSI first, and which one we're currently trying.
static int g_wmCand[AppConfig::kMaxSavedNetworks];
static int g_wmCandCount = 0;
static int g_wmTry = -1;            // index into g_wmCand; -1 => (re)scan needed
static uint32_t g_wmAttemptMs = 0;
static uint32_t g_nextOtaCheckMs = 0; // when to next run the OTA manifest auto-check

int webPortalSavedCount() { return cfg.savedNetworkCount; }

bool webPortalSavedNetwork(int i, char *ssid, size_t cap) {
    if (i < 0 || i >= cfg.savedNetworkCount || cap == 0) return false;
    strncpy(ssid, cfg.savedNetworks[i].ssid, cap - 1);
    ssid[cap - 1] = '\0';
    return true;
}

bool webPortalAddNetwork(const char *ssid, const char *password) {
    if (!ssid || !ssid[0]) return false;
    int slot = -1;
    for (int i = 0; i < cfg.savedNetworkCount; i++)
        if (strncmp(cfg.savedNetworks[i].ssid, ssid, 31) == 0) { slot = i; break; }
    if (slot < 0) {
        if (cfg.savedNetworkCount >= AppConfig::kMaxSavedNetworks)
            slot = AppConfig::kMaxSavedNetworks - 1;  // list full: replace the last
        else
            slot = cfg.savedNetworkCount++;
    }
    strncpy(cfg.savedNetworks[slot].ssid, ssid, sizeof(cfg.savedNetworks[slot].ssid) - 1);
    cfg.savedNetworks[slot].ssid[sizeof(cfg.savedNetworks[slot].ssid) - 1] = '\0';
    strncpy(cfg.savedNetworks[slot].password, password ? password : "",
            sizeof(cfg.savedNetworks[slot].password) - 1);
    cfg.savedNetworks[slot].password[sizeof(cfg.savedNetworks[slot].password) - 1] = '\0';
    saveConfigToNVS(cfg);
    g_wmTry = -1; g_wmAttemptMs = 0;   // force a fresh manager cycle to try it now
    g_staReconnectReq = true;
    Serial.printf("[wm] saved network \"%s\" (slot %d, %d total)\n", ssid, slot, cfg.savedNetworkCount);
    return true;
}

bool webPortalDeleteNetwork(int idx) {
    if (idx < 0 || idx >= cfg.savedNetworkCount) return false;
    for (int i = idx; i < cfg.savedNetworkCount - 1; i++)
        cfg.savedNetworks[i] = cfg.savedNetworks[i + 1];
    cfg.savedNetworkCount--;
    cfg.savedNetworks[cfg.savedNetworkCount].ssid[0] = '\0';
    cfg.savedNetworks[cfg.savedNetworkCount].password[0] = '\0';
    saveConfigToNVS(cfg);
    g_wmTry = -1; g_wmAttemptMs = 0;
    g_staReconnectReq = true;
    Serial.printf("[wm] deleted saved network #%d (%d left)\n", idx, cfg.savedNetworkCount);
    return true;
}

// wifiEnabledRequest: what the UI wants (written by webPortalRequestEnable(),
// read by webTaskFn()). wifiActuallyEnabled: what's actually been applied
// (written only by applyWifiState(), inside the web task). Two separate
// flags rather than one, so a request can be safely set from any task
// (Core 1's touch handling included) while every actual WiFi.*/server.*()
// call still only ever happens from the web task's own context — mixing
// those from two tasks is the kind of thing that's fine 99% of the time and
// then hangs mysteriously the 1% of the time two cores hit the WiFi driver
// at once. Plain bool, not a mutex-protected SharedState-style snapshot:
// same reasoning AppConfig.h gives for its own lock-free cross-task reads —
// a single bool has no torn/garbage intermediate value to race on.
static volatile bool wifiEnabledRequest = false;
static volatile bool wifiActuallyEnabled = false;

void webPortalRequestEnable(bool on) { wifiEnabledRequest = on; }
bool webPortalIsEnabled() { return wifiActuallyEnabled; }
int webPortalClientCount() { return wifiActuallyEnabled ? (int)WiFi.softAPgetStationNum() : 0; }

void webPortalStatusText(char *buf, size_t cap) {
    if (wifiActuallyEnabled)
        snprintf(buf, cap, "%s  %s  %d may", g_apSsid, WiFi.softAPIP().toString().c_str(),
                 (int)WiFi.softAPgetStationNum());
    else
        snprintf(buf, cap, "OFF");
}

// The AP SSID this device uses — a user-set custom name, else "VietHUD-XXXX"
// (last 2 MAC bytes). Computed the SAME way applyWifiState() sets g_apSsid, so
// it's correct even before the AP has been brought up (the QR setup screen needs
// it to build the join code). See Settings.cpp's wifiQrRebuild().
void webPortalApSsid(char *buf, size_t cap) {
    if (cap == 0) return;
    if (cfg.wifiSsid[0] && strcmp(cfg.wifiSsid, "VietHUD") != 0) {
        strncpy(buf, cfg.wifiSsid, cap - 1);
        buf[cap - 1] = '\0';
    } else {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(buf, cap, "VietHUD-%02X%02X", mac[4], mac[5]);
    }
}

// The device's IP on the joined WiFi (station), for prominent on-screen display
// after a successful connect — the user can then reach the web portal at this
// address (or http://viethud.local) from any device on the same network. Returns
// false (buf empty) when the station isn't connected.
bool webPortalStaIp(char *buf, size_t cap) {
    if (cap == 0) return false;
    buf[0] = '\0';
    if (WiFi.status() != WL_CONNECTED) return false;
    IPAddress ip = WiFi.localIP();
    if (ip == IPAddress(0, 0, 0, 0)) return false;
    strncpy(buf, ip.toString().c_str(), cap - 1);
    buf[cap - 1] = '\0';
    return true;
}

// STA (internet) connection line for the on-screen Settings + web. Decodes the
// station status so a failure is diagnosable at a glance (the iPhone-hotspot
// 5 GHz gotcha shows up here as "khong thay mang").
void webPortalStaInfo(char *buf, size_t cap) {
    if (cfg.staSsid[0] == '\0') {
        snprintf(buf, cap, "Tat (chua dat mang)");
        return;
    }
    if (!wifiActuallyEnabled) {
        snprintf(buf, cap, "Cho bat WiFi");
        return;
    }
    wl_status_t wl = WiFi.status();
    if (wl == WL_CONNECTED) {
        snprintf(buf, cap, "Da noi %s  %s  %ddBm%s", cfg.staSsid, WiFi.localIP().toString().c_str(),
                 (int)WiFi.RSSI(), ntpSynced ? "  NTP" : "");
    } else if (wl == WL_NO_SSID_AVAIL) {
        snprintf(buf, cap, "Khong thay \"%s\" (5GHz/tat?)", cfg.staSsid);
    } else if (wl == WL_CONNECT_FAILED) {
        snprintf(buf, cap, "Sai mat khau \"%s\"?", cfg.staSsid);
    } else {
        snprintf(buf, cap, "Dang ket noi \"%s\"...", cfg.staSsid);
    }
}

bool webPortalLocalTime(int *hour, int *minute) {
    if (!ntpSynced) return false;
    time_t now = time(nullptr);
    if (now < 1609459200) return false; // < 2021-01-01 => clock not really set yet
    struct tm lt;
    localtime_r(&now, &lt); // configTime() below sets the VN UTC+7 offset, so this is local
    if (hour) *hour = lt.tm_hour;
    if (minute) *minute = lt.tm_min;
    return true;
}

// ---------------------------------------------------------------------
// "/" — live telemetry page. Static HTML/CSS/JS (PROGMEM, no per-request
// formatting needed) that polls /api/status every 500ms and fills in the
// DOM — same cadence Settings.cpp's own refreshSensorsPanel() timer uses
// for the same reasoning: live values only need to be as fresh as a human
// reads them, no need for anything tighter.
// ---------------------------------------------------------------------
// The page itself lives in net/PortalPage.h (mobile-first SPA: Dữ liệu /
// Trạng thái / Wi-Fi / Hệ thống tabs + the Phone Update Bridge client).
static void handleIndex() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kPortalHtml);
}

// ---------------------------------------------------------------------
// "/api/status" — hand-rolled JSON (no ArduinoJson dependency — same
// reasoning as everywhere else in this project that a small, fixed set of
// fields is simpler formatted directly than pulling in a library for it).
// Static buffer, not stack-local: webTaskFn is the only task that ever
// calls into this server, so there's no concurrent-write hazard, and a
// ~1.5KB buffer is safer off a FreeRTOS task's stack than on it.
// ---------------------------------------------------------------------
// Bumped 1536->1792 (2026-09-21) alongside adding the "camera" object below
// — a comfortable margin over measured worst case, not a tight fit.
static char statusBuf[2560];

static void handleApiStatus() {
    GnssSnapshot gnss = gnssSnapshot();
    RoadInfoSnapshot road = roadInfoSnapshot();
    DataUpdateStatus du = dataUpdateGetStatus();

    // Speed-camera-ahead (user-requested 2026-09-21, "them the hien phia
    // truoc co camera") — same map/SpeedLimitManager.cpp's matchCameraAhead()
    // result the Dashboard's own alert card already shows on-device (see
    // ui/Dashboard.cpp's updateAlertCard()); this is the same data, just
    // exposed to the web Live page too, which polls /api/status but never
    // had any RoadInfoSnapshot field in it before this.
    char cameraBuf[96] = "null";
    if (road.cameraAheadValid) {
        snprintf(cameraBuf, sizeof(cameraBuf), "{\"distanceM\":%.0f,\"speedLimitKmh\":%s}",
                 (double)road.cameraAheadDistanceM,
                 road.cameraSpeedLimitKmh >= 0 ? String(road.cameraSpeedLimitKmh, 0).c_str() : "null");
    }

    size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t minFreeInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    // Compass direction letter from the current heading (same 8-way mapping the
    // Dashboard's heading readout uses).
    static const char *kDir8[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const char *dir = gnss.headingValid ? kDir8[((int)lroundf(gnss.headingDeg / 45.0f)) % 8 & 7] : "--";

    snprintf(statusBuf, sizeof(statusBuf),
             "{\"uptimeMs\":%lu,"
             "\"boardTempC\":%.1f,"
             "\"gnss\":{\"fix\":%s,\"linkAlive\":%s,\"satCount\":%d,\"speedKmh\":%.1f,\"rawSpeedKmh\":%.1f,"
             "\"headingValid\":%s,\"headingDeg\":%.0f,\"dir\":\"%s\",\"timeValid\":%s,\"utcHour\":%d,\"utcMinute\":%d},"
             "\"speedMap\":{\"loaded\":%s,\"limitValid\":%s,\"limitKmh\":%.0f},"
             "\"cameraAhead\":%s,"
             "\"wifi\":{\"on\":%s,\"apSsid\":\"%s\",\"apIp\":\"%s\",\"clients\":%d,\"staSsid\":\"%s\","
             "\"staConnected\":%s,\"staIp\":\"%s\",\"rssi\":%d,\"staStatus\":%d,\"ntp\":%s,\"savedCount\":%d},"
             "\"dataUpdate\":{\"state\":%d,\"filesDone\":%d,\"filesTotal\":%d,\"percent\":%d,\"msg\":\"%s\"},"
             "\"ota\":{\"available\":%s,\"checking\":%s,\"remote\":\"%s\",\"local\":\"%s\"},"
             "\"mem\":{\"freeInternalKB\":%u,\"minFreeInternalKBEver\":%u,\"freePsramKB\":%u}}",
             (unsigned long)millis(), (double)g_boardTempC, gnss.fix ? "true" : "false",
             gnss.linkAlive ? "true" : "false", gnss.satCount, (double)gnss.egoSpeedKmh, (double)gnss.rawSpeedKmh,
             gnss.headingValid ? "true" : "false", (double)gnss.headingDeg, dir,
             gnss.timeValid ? "true" : "false", gnss.utcHour, gnss.utcMinute,
             road.mapLoaded ? "true" : "false", road.valid ? "true" : "false",
             road.valid ? (double)road.speedLimitKmh : 0.0, cameraBuf,
             wifiActuallyEnabled ? "true" : "false", g_apSsid, WiFi.softAPIP().toString().c_str(),
             (int)WiFi.softAPgetStationNum(), cfg.staSsid, staConnected ? "true" : "false",
             staConnected ? WiFi.localIP().toString().c_str() : "", staConnected ? (int)WiFi.RSSI() : 0,
             (int)WiFi.status(), ntpSynced ? "true" : "false", cfg.savedNetworkCount,
             (int)du.state, du.filesDone, du.filesTotal, du.percent, du.message,
             dataUpdateAvailable() ? "true" : "false", dataUpdateCheckInProgress() ? "true" : "false",
             dataUpdateRemoteVersion(), dataUpdateLocalVersion(),
             (unsigned)(freeInternal / 1024), (unsigned)(minFreeInternal / 1024), (unsigned)(ESP.getFreePsram() / 1024));

    server.send(200, "application/json", statusBuf);
}

// ---------------------------------------------------------------------
// "/api/speedlimit" and "/api/speedmap/debug" — spec sections 41/42. Both
// just format whatever map/SpeedLimitManager.h already publishes; neither
// touches the SD card or the matcher directly (spec section 19's "only one
// owner" rule applies here too, not just to the on-device UI).
// ---------------------------------------------------------------------
static char speedLimitBuf[384];

static void handleApiSpeedLimit() {
    RoadInfoSnapshot road = roadInfoSnapshot();
    if (road.valid) {
        snprintf(speedLimitBuf, sizeof(speedLimitBuf),
                 "{\"valid\":true,\"limit\":%.0f,\"unit\":\"km/h\",\"source\":\"%s\",\"confidence\":%.2f,"
                 "\"road_id\":%lu,\"next_limit\":null,\"next_distance\":null}",
                 (double)road.speedLimitKmh, speedSourceStr(road.source), (double)road.confidence,
                 (unsigned long)road.roadId);
    } else {
        // Spec section 41's own example shows just {"valid":false,"limit":null}
        // for the not-valid case — matched here rather than a fuller schema,
        // so "limit" being null is unambiguously "don't know", never 0.
        snprintf(speedLimitBuf, sizeof(speedLimitBuf), "{\"valid\":false,\"limit\":null}");
    }
    server.send(200, "application/json", speedLimitBuf);
}

static char speedMapDebugBuf[512];

static void handleApiSpeedMapDebug() {
    GnssSnapshot gnss = gnssSnapshot();
    RoadInfoSnapshot road = roadInfoSnapshot();
    snprintf(speedMapDebugBuf, sizeof(speedMapDebugBuf),
             "{\"gps\":{\"lat\":%.7f,\"lon\":%.7f,\"headingDeg\":%.0f,\"headingValid\":%s,\"fix\":%s},"
             "\"match\":{\"mapLoaded\":%s,\"roadId\":%lu,\"distanceM\":%.1f,\"confidence\":%.2f},"
             "\"limit\":{\"valid\":%s,\"current\":%s,\"source\":\"%s\"},"
             "\"next\":{\"limit\":null,\"distance\":null}}",
             (double)gnss.latDeg, (double)gnss.lonDeg, (double)gnss.headingDeg,
             gnss.headingValid ? "true" : "false", gnss.fix ? "true" : "false", road.mapLoaded ? "true" : "false",
             (unsigned long)road.roadId, (double)road.matchDistanceM, (double)road.confidence,
             road.valid ? "true" : "false", road.valid ? String(road.speedLimitKmh, 0).c_str() : "null",
             speedSourceStr(road.source));
    server.send(200, "application/json", speedMapDebugBuf);
}

// ---------------------------------------------------------------------
// "/triplog" — lists the drive logs actually on the card, each a download
// link. Added 2026-09-21 after retrieving one real road test's logs the
// painful way (a temporary firmware hack that dumped the CSV over serial,
// several reflashes, one SD-peripheral wedge); with the device's own AP
// already implemented, a phone or laptop at the roadside is a far better
// path off the card than a USB cable and a rebuild.
// ---------------------------------------------------------------------
static const int kMaxListedTripLogs = 64;

static void handleTripLogIndex() {
    uint32_t ids[kMaxListedTripLogs], sizes[kMaxListedTripLogs];
    int n = sdMgrListTripLogs(ids, sizes, kMaxListedTripLogs);

    String page = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                     "<style>body{font-family:sans-serif;margin:16px}a{display:block;padding:10px 0;"
                     "border-bottom:1px solid #ddd}</style><h2>Trip logs</h2>");
    if (n == 0) {
        page += F("<p>No logs found (no SD card, or no drive recorded yet).</p>");
    } else {
        // Newest first: session ids increment once per boot, so a plain
        // descending sort is chronological without needing timestamps
        // (which the CSV itself can't carry reliably anyway — GNSS time
        // isn't trustworthy until a fix, see TripLogger.h).
        for (int pass = 0; pass < n; pass++) {
            int best = -1;
            for (int i = 0; i < n; i++)
                if (sizes[i] != 0xFFFFFFFFu && (best < 0 || ids[i] > ids[best])) best = i;
            if (best < 0) break;
            char row[160];
            snprintf(row, sizeof(row), "<a href='/triplog/get?id=%lu'>session_%04lu.csv &mdash; %lu bytes</a>",
                      (unsigned long)ids[best], (unsigned long)ids[best], (unsigned long)sizes[best]);
            page += row;
            sizes[best] = 0xFFFFFFFFu; // consumed
        }
    }
    server.send(200, "text/html", page);
}

static void handleTripLogGet() {
    if (!server.hasArg("id")) {
        server.send(400, "text/plain", "missing id");
        return;
    }
    unsigned long id = strtoul(server.arg("id").c_str(), NULL, 10);
    char path[48], fname[32];
    snprintf(path, sizeof(path), "/triplog/session_%04lu.csv", id);
    snprintf(fname, sizeof(fname), "session_%04lu.csv", id);

    // Chunked through sdMgrReadFileChunk() rather than a single read into
    // RAM: a long drive's CSV has no fixed upper bound, and this keeps the
    // SD mutex held only per 1KB chunk instead of for the whole transfer
    // (the map/trip-log tasks both share that same card).
    uint8_t buf[1024];
    int first = sdMgrReadFileChunk(path, 0, buf, sizeof(buf));
    if (first < 0) {
        server.send(404, "text/plain", "not found");
        return;
    }
    server.sendHeader("Content-Disposition", String("attachment; filename=") + fname);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/csv", "");
    size_t offset = 0;
    int got = first;
    while (got > 0) {
        server.sendContent((const char *)buf, got);
        offset += got;
        got = sdMgrReadFileChunk(path, offset, buf, sizeof(buf));
    }
    server.sendContent("");
}

// ---------------------------------------------------------------------
// "/config" — GET renders the current AppConfig as an editable form, POST
// parses it back, clamps/sanitizes, applies, and saves to NVS immediately
// (see WebPortal.h's header comment for why this doesn't need Settings.cpp
// touchscreen's confirm-modal step). Field list, ranges and step sizes are
// kept in sync with Settings.cpp's addSliderRow()/addSwitchRow() calls and
// AppConfig.h's clampConfig() by hand — there's no shared table between
// this file and Settings.cpp, so a new AppConfig field needs adding in
// both places, same as NvsStore.cpp already requires today.
// ---------------------------------------------------------------------
static char configBuf[7680];

static size_t appendField(char *buf, size_t cap, size_t len, const char *name, const char *label, float value,
                           float step, float minV, float maxV) {
    return len + snprintf(buf + len, len < cap ? cap - len : 0,
                           "<label>%s<input type=\"number\" step=\"%.3f\" min=\"%.3f\" max=\"%.3f\" name=\"%s\" "
                           "value=\"%.3f\"></label>",
                           label, (double)step, (double)minV, (double)maxV, name, (double)value);
}

static size_t appendCheckbox(char *buf, size_t cap, size_t len, const char *name, const char *label, bool value) {
    return len + snprintf(buf + len, len < cap ? cap - len : 0,
                           "<label class=\"chk\"><input type=\"checkbox\" name=\"%s\"%s>%s</label>", name,
                           value ? " checked" : "", label);
}

// Password field's value is deliberately left BLANK rather than pre-filled
// with cfg.wifiPassword — echoing a saved password back into a form is bad
// practice even on a local-only device, and handleConfigPost() below treats
// a submitted-blank password field as "leave it unchanged" rather than
// "clear it", so leaving it blank here has no destructive side effect.
static size_t appendTextField(char *buf, size_t cap, size_t len, const char *name, const char *label,
                               const char *value, bool isPassword) {
    return len + snprintf(buf + len, len < cap ? cap - len : 0,
                           "<label>%s<input type=\"%s\" name=\"%s\" value=\"%s\"></label>", label,
                           isPassword ? "password" : "text", name, value);
}

static void handleConfigGet() {
    size_t len = 0;
    len += snprintf(configBuf + len, sizeof(configBuf) - len,
                     "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>VietHUD - "
                     "Config</title><style>"
                     "body{background:#0B0F14;color:#CCD6E0;font-family:sans-serif;margin:0;padding:12px}"
                     "h1{font-size:18px;color:#fff}nav a{color:#4AA3FF;margin-right:16px;font-size:13px;"
                     "text-decoration:none}"
                     "fieldset{border:1px solid #2A3441;border-radius:8px;margin-bottom:12px}"
                     "legend{color:#7C8A9A;font-size:12px;text-transform:uppercase}"
                     "label{display:block;margin:8px 0;font-size:13px}"
                     "input[type=number]{width:100px;float:right;background:#151C24;color:#fff;border:1px solid "
                     "#2A3441;border-radius:4px;padding:2px 6px}"
                     "select{width:160px;float:right;background:#151C24;color:#fff;border:1px solid #2A3441;"
                     "border-radius:4px;padding:2px 6px}"
                     "label.chk{display:inline-block;width:48%%}label.chk input{float:none;margin-right:6px}"
                     "button{background:#2E7D4F;color:#fff;border:0;border-radius:6px;padding:10px 20px;"
                     "font-size:14px}"
                     "</style></head><body>"
                     "<nav><a href=\"/\">&lsaquo; Trang ch&iacute;nh</a><a href=\"/config\">C&agrave;i &#273;&#7863;t</a><a href=\"/update\">Firmware</a></nav>"
                     "<h1>VietHUD - Configuration</h1><form method=\"POST\" action=\"/config\">");

    len += snprintf(configBuf + len, sizeof(configBuf) - len, "<fieldset><legend>Alerts</legend>");
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "audioEnabled", "Alert audio enabled", cfg.audioEnabled);
    len = appendField(configBuf, sizeof(configBuf), len, "audioVolume", "Volume (%)", cfg.audioVolume, 1, 0, 100);
    len += snprintf(configBuf + len, sizeof(configBuf) - len, "</fieldset><fieldset><legend>Display</legend>");
    len = appendField(configBuf, sizeof(configBuf), len, "brightness", "Brightness (%)", cfg.brightness, 1, 5, 100);
    len = appendField(configBuf, sizeof(configBuf), len, "autoDimMin", "Auto-dim after (min)", cfg.autoDimMin, 1, 0,
                       30);
    len += snprintf(configBuf + len, sizeof(configBuf) - len, "</fieldset><fieldset><legend>Sensors (GNSS)</legend>");
    len = appendField(configBuf, sizeof(configBuf), len, "gnssSpeedFilterAlpha", "Speed filter smoothing",
                       cfg.gnssSpeedFilterAlpha, 0.01f, 0.05f, 0.90f);
    len = appendField(configBuf, sizeof(configBuf), len, "gnssFixTimeoutS", "Fix timeout (s)", cfg.gnssFixTimeoutS,
                       0.1f, 1, 10);
    len = appendField(configBuf, sizeof(configBuf), len, "gnssSpeedCalibrationPct", "Speed calibration (%)",
                       cfg.gnssSpeedCalibrationPct, 0.1f, -15, 15);
    len = appendField(configBuf, sizeof(configBuf), len, "overspeedOffsetKmh", "Overspeed warning offset (km/h)",
                       cfg.overspeedOffsetKmh, 1, 0, 10);
    len = appendField(configBuf, sizeof(configBuf), len, "defaultLimitKmh", "Default limit when unknown (km/h, 0=off)",
                       cfg.defaultLimitKmh, 1, 0, 90);
    // (aheadLimitWarnDistM / cameraWarnDistM removed 2026-09-24 — never used;
    // the lookahead uses a dynamic speed-based warn distance.)
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "tripLoggingEnabled", "Trip logging (SD card)",
                          cfg.tripLoggingEnabled);
    len += snprintf(configBuf + len, sizeof(configBuf) - len, "</fieldset><fieldset><legend>WiFi hotspot (AP)</legend>");
    len = appendTextField(configBuf, sizeof(configBuf), len, "wifiSsid", "SSID", cfg.wifiSsid, false);
    len = appendTextField(configBuf, sizeof(configBuf), len, "wifiPassword", "New password (blank = keep current)",
                           "", true);
    len = appendField(configBuf, sizeof(configBuf), len, "wifiAutoOffMin", "Auto-off after (min, 0=never)",
                       cfg.wifiAutoOffMin, 1, 0, 120);
    len += snprintf(configBuf + len, sizeof(configBuf) - len,
                     "<p style=\"font-size:11px;color:#7C8A9A\">On/off is on the device screen only (Settings &gt; "
                     "WiFi, or hold the Dashboard 3s+) — not here, since submitting an off request over WiFi would "
                     "disconnect this page mid-request.</p></fieldset>"
                     "<fieldset><legend>Internet (Station / NTP)</legend>"
                     "<p style=\"font-size:11px;color:#7C8A9A\">Optionally join your phone hotspot / home WiFi so the "
                     "device can sync the clock over the internet (NTP) without waiting for GPS. Leave SSID blank for "
                     "hotspot-only. Applied on the next WiFi on/off.</p>");
    len = appendTextField(configBuf, sizeof(configBuf), len, "staSsid", "Network SSID (blank = off)", cfg.staSsid, false);
    len = appendTextField(configBuf, sizeof(configBuf), len, "staPassword", "Network password (blank = keep current)",
                           "", true);
    len += snprintf(configBuf + len, sizeof(configBuf) - len,
                     "</fieldset><fieldset><legend>Online data update</legend>"
                     "<p style=\"font-size:11px;color:#7C8A9A\">Base URL serving the map/warning data + "
                     "manifest.txt (your Raspberry Pi). The Live page's \"Update data\" button pulls from here.</p>");
    len = appendTextField(configBuf, sizeof(configBuf), len, "dataUpdateUrl", "Data URL (e.g. https://host/speedmap/)",
                           cfg.dataUpdateUrl, false);
    len += snprintf(configBuf + len, sizeof(configBuf) - len,
                     "</fieldset><button type=\"submit\">Save to device</button></form></body></html>");

    server.send(200, "text/html", configBuf);
}

static float argFloat(const char *name, float fallback) {
    if (!server.hasArg(name)) return fallback;
    return server.arg(name).toFloat();
}

static void handleConfigPost() {
    cfg.brightness = argFloat("brightness", cfg.brightness);
    cfg.audioVolume = argFloat("audioVolume", cfg.audioVolume);
    cfg.autoDimMin = argFloat("autoDimMin", cfg.autoDimMin);
    cfg.gnssSpeedFilterAlpha = argFloat("gnssSpeedFilterAlpha", cfg.gnssSpeedFilterAlpha);
    cfg.gnssFixTimeoutS = argFloat("gnssFixTimeoutS", cfg.gnssFixTimeoutS);
    cfg.gnssSpeedCalibrationPct = argFloat("gnssSpeedCalibrationPct", cfg.gnssSpeedCalibrationPct);
    cfg.overspeedOffsetKmh = argFloat("overspeedOffsetKmh", cfg.overspeedOffsetKmh);
    cfg.defaultLimitKmh = argFloat("defaultLimitKmh", cfg.defaultLimitKmh);
    if (server.hasArg("wifiSsid")) {
        strncpy(cfg.wifiSsid, server.arg("wifiSsid").c_str(), sizeof(cfg.wifiSsid) - 1);
        cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0';
    }
    // Blank submitted password means "leave it unchanged" — see appendTextField()'s comment.
    if (server.hasArg("wifiPassword") && server.arg("wifiPassword").length() > 0) {
        strncpy(cfg.wifiPassword, server.arg("wifiPassword").c_str(), sizeof(cfg.wifiPassword) - 1);
        cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0';
    }
    cfg.wifiAutoOffMin = argFloat("wifiAutoOffMin", cfg.wifiAutoOffMin);
    if (server.hasArg("staSsid")) { // may be intentionally blank (= station off)
        strncpy(cfg.staSsid, server.arg("staSsid").c_str(), sizeof(cfg.staSsid) - 1);
        cfg.staSsid[sizeof(cfg.staSsid) - 1] = '\0';
    }
    if (server.hasArg("staPassword") && server.arg("staPassword").length() > 0) { // blank = keep current
        strncpy(cfg.staPassword, server.arg("staPassword").c_str(), sizeof(cfg.staPassword) - 1);
        cfg.staPassword[sizeof(cfg.staPassword) - 1] = '\0';
    }
    if (server.hasArg("dataUpdateUrl")) { // may be intentionally blank (= updater off)
        strncpy(cfg.dataUpdateUrl, server.arg("dataUpdateUrl").c_str(), sizeof(cfg.dataUpdateUrl) - 1);
        cfg.dataUpdateUrl[sizeof(cfg.dataUpdateUrl) - 1] = '\0';
    }
    // Checkboxes only appear in POST data when checked — an absent arg means unchecked, not "leave unchanged".
    cfg.audioEnabled = server.hasArg("audioEnabled");
    cfg.tripLoggingEnabled = server.hasArg("tripLoggingEnabled");

    sanitizeConfig(cfg);
    clampConfig(cfg);
    applyConfig(); // brightness may have changed — same PWM rewrite Settings.cpp's slider triggers
    saveConfigToNVS(cfg);
    Serial.println("[web] config updated + saved to NVS via /config");

    server.sendHeader("Location", "/config");
    server.send(303); // redirect back so a page refresh doesn't resubmit the form
}

// ---------------------------------------------------------------------
// "/update" — standard ESP32-Arduino HTTP OTA upload (Update.h, built into
// the core). GET shows a plain upload form; POST's multipart body is fed
// into Update.write() as it streams in, then the device reboots into the
// new firmware. Same risk profile as any OTA mechanism: an interrupted
// upload can leave a bad image, which is why Update.end(true) is checked
// before claiming success and the device reboots either way to get back to
// a known state.
// ---------------------------------------------------------------------
static const char kUpdateHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VietHUD - OTA Update</title>
<style>body{background:#0B0F14;color:#CCD6E0;font-family:sans-serif;margin:0;padding:12px}
h1{font-size:18px;color:#fff}nav a{color:#4AA3FF;margin-right:16px;font-size:13px;text-decoration:none}
button{background:#2E7D4F;color:#fff;border:0;border-radius:6px;padding:10px 20px;font-size:14px}
p.warn{color:#E0C020}</style></head><body>
<nav><a href="/">&lsaquo; Trang ch&iacute;nh</a><a href="/triplog">Trip logs</a><a href="/config">C&agrave;i &#273;&#7863;t</a></nav>
<h1>VietHUD - OTA Firmware Update</h1>
<p class="warn">Upload a .bin built for env:viethud. Do not power off during upload — the device reboots automatically when done.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update" accept=".bin"><br><br>
<button type="submit">Upload and flash</button>
</form></body></html>)HTML";

static void handleUpdateGet() { server.send_P(200, "text/html", kUpdateHtml); }

static void handleUpdateUpload() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[web] OTA upload start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[web] OTA success: %u bytes written\n", (unsigned)upload.totalSize);
        else Update.printError(Serial);
    }
}

static void handleUpdatePost() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "OTA FAILED — see serial log" : "OTA OK — rebooting...");
    delay(500);
    ESP.restart();
}

// ---------------------------------------------------------------------
// "/api/action" (POST ?do=...) — the Live page's control panel (feature G).
// Small, explicit actions only; runs in the web task's context so it can call
// audio/demo/SD helpers directly. Every action is idempotent-ish and safe to
// fire from a phone. Reboot sends its reply first, then restarts after a beat.
// ---------------------------------------------------------------------
static void handleApiAction() {
    String a = server.arg("do");
    if (a == "audiotest") {
        audioSelfTest();
        server.send(200, "text/plain", "Audio self-test started");
    } else if (a == "demo") {
        bool on = !demoModeIsEnabled();
        demoModeSetEnabled(on);
        server.send(200, "text/plain", on ? "Demo mode ON" : "Demo mode OFF");
    } else if (a == "clearlogs") {
        int n = sdMgrDeleteAllTripLogs();
        char msg[48];
        snprintf(msg, sizeof(msg), n < 0 ? "SD not available" : "Deleted %d trip log(s)", n);
        server.send(200, "text/plain", msg);
    } else if (a == "dataupdate") {
        if (cfg.dataUpdateUrl[0] == '\0') {
            server.send(200, "text/plain", "No update URL set (Config)");
        } else {
            server.send(200, "text/plain", "Rebooting into update mode...");
            delay(300);
            dataUpdateSchedule(); // sets NVS flag + reboots; download runs at next boot with RAM free for TLS
        }
    } else if (a == "otacheck") {
        server.send(200, "text/plain",
                    dataUpdateCheckStart() ? "Checking for update..." : "Cannot check (WiFi off / no URL / busy)");
    } else if (a == "reboot") {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", "Rebooting...");
        delay(400);
        ESP.restart();
    } else {
        server.send(400, "text/plain", "unknown action");
    }
}

// ---- WiFi Manager web endpoints (2026-09-26) ----
static String jsonEsc(const char *s) {
    String o = s;
    o.replace("\\", "\\\\");
    o.replace("\"", "\\\"");
    return o;
}
static void handleWifiScan() {
    if (server.hasArg("rescan")) webPortalStartScan();
    int st = webPortalScanState();
    String out = "{\"state\":" + String(st) + ",\"results\":[";
    if (st >= 0) {
        for (int i = 0; i < st; i++) {
            char ss[33]; int rssi = 0; bool locked = false;
            webPortalScanResult(i, ss, sizeof(ss), &rssi, &locked);
            if (i) out += ",";
            out += "{\"ssid\":\"" + jsonEsc(ss) + "\",\"rssi\":" + String(rssi) +
                   ",\"locked\":" + (locked ? "true" : "false") + "}";
        }
    }
    out += "]}";
    server.send(200, "application/json", out);
}
static void handleWifiSaved() {
    String out = "[";
    int n = webPortalSavedCount();
    for (int i = 0; i < n; i++) {
        char ss[33]; webPortalSavedNetwork(i, ss, sizeof(ss));
        if (i) out += ",";
        out += "{\"i\":" + String(i) + ",\"ssid\":\"" + jsonEsc(ss) + "\"}";
    }
    out += "]";
    server.send(200, "application/json", out);
}
static void handleWifiAdd() {
    String ssid = server.arg("ssid");
    if (ssid.length() == 0) { server.send(400, "text/plain", "ssid required"); return; }
    bool ok = webPortalAddNetwork(ssid.c_str(), server.arg("password").c_str());
    server.send(ok ? 200 : 400, "text/plain", ok ? "Saved. Connecting..." : "Failed (list full?)");
}
static void handleWifiDel() {
    bool ok = webPortalDeleteNetwork(server.arg("idx").toInt());
    server.send(ok ? 200 : 400, "text/plain", ok ? "Deleted" : "Failed");
}

// Captive-portal catch-all (feature E): any URL the WebServer doesn't have a
// route for gets a redirect to the portal root, so connecting to the AP pops
// the page open (phones probe a handful of "is there internet?" URLs on
// join; answering them with a 302 to us is what triggers the captive sign-in).
static void handleCaptiveRedirect() {
    server.sendHeader("Location", String("http://") + kApIp.toString(), true);
    server.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------
// Applies a WiFi on/off transition — the ONLY place in this file that calls
// WiFi.*()/server.begin()/server.stop(), and only ever from webTaskFn()'s
// own context (see wifiEnabledRequest's comment above for why that
// matters). A same-target call (already on, asked for on again) is a no-op.
// Synchronous 2.4GHz scan into the shared g_scan* buffers (web task only). The
// STA is briefly dropped so a station stuck associating to a missing hotspot
// doesn't make the scan return 0 results. Returns the number of visible networks.
static int doScan() {
    Serial.println("[web] scanning nearby WiFi (2.4GHz)...");
    esp_task_wdt_reset();
    WiFi.disconnect(false, false);
    delay(120);
    int n = WiFi.scanNetworks(false /*sync*/, false /*skip hidden*/);
    esp_task_wdt_reset();
    int m = 0;
    for (int i = 0; i < n && m < SCAN_MAX; i++) {
        String ss = WiFi.SSID(i);
        if (ss.length() == 0) continue;
        strncpy(g_scanSsid[m], ss.c_str(), 32);
        g_scanSsid[m][32] = '\0';
        g_scanRssi[m] = (int8_t)WiFi.RSSI(i);
        g_scanLocked[m] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
        m++;
    }
    g_scanCount = m;
    WiFi.scanDelete();
    Serial.printf("[web] scan done: %d network(s)\n", m);
    return m;
}

// WiFi Manager tick — called each loop while WiFi is on. Connects to the
// strongest SAVED network currently in range, rotating to the next candidate
// after ~12s without a link, and re-scanning once all candidates are exhausted.
// Sets cfg.staSsid/staPassword to whichever network is being attempted so the
// existing status/diagnostic code keeps reporting the active network.
static void wmTick() {
    if (cfg.savedNetworkCount <= 0) return;         // nothing to manage (AP only)
    // A phone is pushing data through the AP right now: a scan (2-4 s off-channel)
    // or an STA connect (moves the AP channel) would drop the transfer. Hold off.
    if (updateApiBusy()) return;
    if (WiFi.status() == WL_CONNECTED) { g_wmTry = -1; g_wmAttemptMs = 0; return; }  // linked; reconnect fast if it drops
    uint32_t now = millis();
    // Rate-limit: while attempting a candidate, wait 12s for it to link; between
    // (re)scans when nothing was connectable, back off 15s so we don't scan
    // back-to-back (each scan is ~2-4s and disturbs the AP).
    uint32_t waitMs = (g_wmTry >= 0) ? 12000 : 15000;
    if (g_wmAttemptMs != 0 && (uint32_t)(now - g_wmAttemptMs) < waitMs) return;

    if (g_wmTry < 0 || g_wmTry + 1 >= g_wmCandCount) {
        // (Re)build the candidate list: saved networks present in a fresh scan,
        // ordered by RSSI (strongest first).
        int m = doScan();
        int candRssi[AppConfig::kMaxSavedNetworks];
        g_wmCandCount = 0;
        for (int i = 0; i < cfg.savedNetworkCount; i++) {
            int best = -999;
            bool present = false;
            for (int j = 0; j < m; j++) {
                if (strncmp(cfg.savedNetworks[i].ssid, g_scanSsid[j], 31) == 0) {
                    present = true;
                    if (g_scanRssi[j] > best) best = g_scanRssi[j];
                }
            }
            if (present) {
                g_wmCand[g_wmCandCount] = i;
                candRssi[g_wmCandCount] = best;
                g_wmCandCount++;
            }
        }
        // insertion sort by RSSI desc (n <= 5)
        for (int a = 1; a < g_wmCandCount; a++) {
            int vi = g_wmCand[a], vr = candRssi[a], b = a - 1;
            while (b >= 0 && candRssi[b] < vr) {
                g_wmCand[b + 1] = g_wmCand[b]; candRssi[b + 1] = candRssi[b]; b--;
            }
            g_wmCand[b + 1] = vi; candRssi[b + 1] = vr;
        }
        g_wmTry = -1;
        if (g_wmCandCount == 0) {
            static uint32_t sLastNoneMs = 0;
            if (now - sLastNoneMs > 10000) {
                sLastNoneMs = now;
                Serial.printf("[wm] none of %d saved network(s) in range\n", cfg.savedNetworkCount);
            }
            g_wmAttemptMs = now;   // back off before the next scan
            return;
        }
    }

    g_wmTry++;
    if (g_wmTry >= g_wmCandCount) g_wmTry = 0;
    int ni = g_wmCand[g_wmTry];
    strncpy(cfg.staSsid, cfg.savedNetworks[ni].ssid, sizeof(cfg.staSsid) - 1);
    cfg.staSsid[sizeof(cfg.staSsid) - 1] = '\0';
    strncpy(cfg.staPassword, cfg.savedNetworks[ni].password, sizeof(cfg.staPassword) - 1);
    cfg.staPassword[sizeof(cfg.staPassword) - 1] = '\0';
    if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
    WiFi.begin(cfg.staSsid, cfg.staPassword);
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
    staConnected = false;
    ntpSynced = false;
    g_wmAttemptMs = now;
    Serial.printf("[wm] trying \"%s\" (candidate %d/%d)\n", cfg.staSsid, g_wmTry + 1, g_wmCandCount);
}

static void applyWifiState(bool enable) {
    if (enable == wifiActuallyEnabled) return;
    if (enable) {
        // AP + optional STATION (feature F): if the user configured a station
        // SSID, join it too (AP_STA) for internet/NTP; otherwise plain AP.
        // WiFi Manager: if any network is saved, come up in AP_STA and let
        // wmTick() scan + connect to the strongest saved network in range.
        bool wantSta = cfg.savedNetworkCount > 0 || cfg.staSsid[0] != '\0';
        WiFi.mode(wantSta ? WIFI_AP_STA : WIFI_AP);
        // No modem sleep while WiFi is on: with it, STA round-trips swing 30-190 ms
        // (DTIM wake-ups) and TCP transfers crawl at ~20-40 KB/s. The device is on
        // car power and WiFi is only on while someone is configuring/updating.
        WiFi.setSleep(false);
        // AP SSID: a user-set custom name, else "VietHUD-XXXX" (last 4 MAC hex)
        // so multiple units don't collide (spec 2.1). "VietHUD" alone counts as
        // "not customised" and gets the MAC suffix too.
        char apSsid[40];
        if (cfg.wifiSsid[0] && strcmp(cfg.wifiSsid, "VietHUD") != 0) {
            strncpy(apSsid, cfg.wifiSsid, sizeof(apSsid) - 1);
            apSsid[sizeof(apSsid) - 1] = '\0';
        } else {
            uint8_t mac[6];
            WiFi.macAddress(mac);
            snprintf(apSsid, sizeof(apSsid), "VietHUD-%02X%02X", mac[4], mac[5]);
        }
        strncpy(g_apSsid, apSsid, sizeof(g_apSsid) - 1);
        g_apSsid[sizeof(g_apSsid) - 1] = '\0';
        const char *ssid = apSsid;
        size_t pwLen = strlen(cfg.wifiPassword);
        bool secured = pwLen >= 8; // WPA2 minimum — WiFi.softAP() silently fails to secure below this
        bool ok = secured ? WiFi.softAP(ssid, cfg.wifiPassword) : WiFi.softAP(ssid);
        if (!secured) {
            Serial.printf("[web] WARN: password %s (%u chars, WPA2 needs >=8) — starting an OPEN (unsecured) AP\n",
                          pwLen == 0 ? "empty" : "too short", (unsigned)pwLen);
        }
        if (wantSta) {
            // VN UTC+7; NTP daemon fills the system clock in the background once
            // the station associates — webPortalLocalTime() reads it afterwards.
            configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
            g_wmTry = -1; g_wmAttemptMs = 0;  // trigger wmTick() to scan + connect the best saved net
            Serial.printf("[web] STA manager armed (%d saved network(s))\n", cfg.savedNetworkCount);
        }
        server.begin();
        MDNS.end();                 // in case a stale instance is lingering
        if (MDNS.begin(kMdnsHost)) { // feature B: http://viethud.local
            MDNS.addService("http", "tcp", 80);
            Serial.printf("[web] mDNS up: http://%s.local\n", kMdnsHost);
        }
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(53, "*", kApIp); // feature E: captive portal — resolve everything to us
        Serial.printf("[web] WiFi ON — AP \"%s\" (%s) %s, IP=%s%s\n", ssid, secured ? "secured" : "OPEN",
                      ok ? "up" : "FAILED to start", WiFi.softAPIP().toString().c_str(),
                      wantSta ? " (+STA)" : "");
    } else {
        dnsServer.stop();
        MDNS.end();
        server.stop();
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        staConnected = false;
        ntpSynced = false;
        Serial.println("[web] WiFi OFF");
    }
    wifiActuallyEnabled = enable;
}

static void webTaskFn(void *) {
    // Routes are registered once up front regardless of WiFi state — the
    // WebServer object itself is harmless idle weight while off (nothing
    // can reach it with no radio/AP up), and registering once here avoids
    // re-registering the same handlers on every on/off cycle.
    server.on("/", HTTP_GET, handleIndex);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/speedlimit", HTTP_GET, handleApiSpeedLimit);
    server.on("/api/speedmap/debug", HTTP_GET, handleApiSpeedMapDebug);
    server.on("/triplog", HTTP_GET, handleTripLogIndex);
    server.on("/triplog/get", HTTP_GET, handleTripLogGet);
    server.on("/config", HTTP_GET, handleConfigGet);
    server.on("/config", HTTP_POST, handleConfigPost);
    server.on("/update", HTTP_GET, handleUpdateGet);
    server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
    server.on("/api/action", HTTP_POST, handleApiAction);       // feature G: control panel
    server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);      // WiFi Manager: scan
    server.on("/api/wifi/saved", HTTP_GET, handleWifiSaved);    // WiFi Manager: list saved
    server.on("/api/wifi/add", HTTP_POST, handleWifiAdd);       // WiFi Manager: add/update
    server.on("/api/wifi/del", HTTP_POST, handleWifiDel);       // WiFi Manager: delete
    // Common captive-portal probe URLs + a catch-all, all 302 -> portal root.
    server.on("/generate_204", HTTP_GET, handleCaptiveRedirect);       // Android
    server.on("/gen_204", HTTP_GET, handleCaptiveRedirect);           // Android (older)
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect); // iOS/macOS
    server.on("/ncsi.txt", HTTP_GET, handleCaptiveRedirect);          // Windows
    server.onNotFound(handleCaptiveRedirect);                         // feature E
    updateApiRegister(server);                                        // Phone Update Bridge (/api/v1/*)

    esp_task_wdt_add(NULL);
    uint32_t lastClientMs = millis();  // feature D: auto-off timer baseline
    for (;;) {
        if (wifiEnabledRequest != wifiActuallyEnabled) {
            applyWifiState(wifiEnabledRequest);
            lastClientMs = millis(); // reset the idle timer on every on transition
        }
        // WiFi scan for the on-screen setup overlay — needs the radio, and STA
        // reconnect with freshly-entered creds. Both here so all WiFi.* stays on
        // this task. Scan works in AP_STA; it briefly disturbs the AP, fine for
        // a setup moment.
        if (wifiActuallyEnabled) {
            if (g_scanReq && !updateApiBusy()) {
                g_scanReq = false;
                doScan();                 // fills g_scan* for the UI picker
                g_wmTry = -1; g_wmAttemptMs = 0;  // let wmTick() reconnect after the scan drop
            }
            if (g_staReconnectReq) {
                g_staReconnectReq = false;
                g_wmTry = -1; g_wmAttemptMs = 0;  // force a fresh manager cycle (new/changed creds)
            }
            // WiFi Manager: keep the strongest saved network connected.
            wmTick();
        }
        if (wifiActuallyEnabled) {
            dnsServer.processNextRequest(); // feature E: captive portal DNS
            server.handleClient();
            updateApiLoop();                // deferred reboot after a staged data install

            // Feature F: track the station link, sync NTP once associated.
            wl_status_t wl = WiFi.status();
            bool sta = (wl == WL_CONNECTED);
            if (sta != staConnected) {
                staConnected = sta;
                Serial.printf("[web] STA %s%s\n", sta ? "connected, IP=" : "disconnected",
                              sta ? WiFi.localIP().toString().c_str() : "");
                if (sta) {
                    // OTA auto-check on connect: probe the remote manifest version
                    // (downloads nothing else, never reboots). Give the link a
                    // moment to settle + NTP; a small delay before the first check
                    // is handled by the periodic timer below rather than blocking here.
                    g_nextOtaCheckMs = millis() + 8000;
                }
            }
            // OTA auto-check: on connect (above) and then every ~30 min while online.
            if (sta && cfg.dataUpdateUrl[0] && (int32_t)(millis() - g_nextOtaCheckMs) >= 0) {
                if (dataUpdateCheckStart())
                    g_nextOtaCheckMs = millis() + 30UL * 60UL * 1000UL;
                else
                    g_nextOtaCheckMs = millis() + 60UL * 1000UL; // busy/not-ready: retry in 1 min
            }
            // Diagnostic: while a station is configured but NOT connected, log the
            // reason every ~5s. WL_NO_SSID_AVAIL usually means the hotspot is off,
            // out of range, the name doesn't match, OR it's 5 GHz-only (an iPhone
            // hotspot needs "Maximize Compatibility" ON to broadcast 2.4 GHz —
            // the ESP32-S3 radio is 2.4 GHz only). WL_CONNECT_FAILED = wrong pass.
            if (cfg.staSsid[0] && !sta) {
                static uint32_t sLastStaDiag = 0;
                if (millis() - sLastStaDiag > 5000) {
                    sLastStaDiag = millis();
                    const char *r = wl == WL_NO_SSID_AVAIL   ? "SSID not found (off/out-of-range/5GHz/name)"
                                    : wl == WL_CONNECT_FAILED ? "connect failed (wrong password?)"
                                    : wl == WL_IDLE_STATUS    ? "idle"
                                    : wl == WL_DISCONNECTED   ? "disconnected/associating"
                                                              : "status?";
                    Serial.printf("[web] STA \"%s\" not connected: status=%d (%s)\n", cfg.staSsid, (int)wl, r);
                }
            }
            if (sta && !ntpSynced && time(nullptr) > 1609459200) { // NTP delivered a real epoch
                ntpSynced = true;
                Serial.println("[web] NTP time synced");
            }

            // Feature D: auto-off after wifiAutoOffMin with no AP client.
            if (WiFi.softAPgetStationNum() > 0 || updateApiBusy()) lastClientMs = millis();
            uint32_t idleLimitMs = (uint32_t)(cfg.wifiAutoOffMin * 60000.0f);
            if (idleLimitMs > 0 && millis() - lastClientMs > idleLimitMs) {
                Serial.printf("[web] WiFi auto-off: no client for %.0f min\n", (double)cfg.wifiAutoOffMin);
                wifiEnabledRequest = false; // applied on the next loop iteration
            }
        }
        esp_task_wdt_reset();
        // No need to poll fast while off — nothing to service until someone
        // asks for WiFi again.
        vTaskDelay(pdMS_TO_TICKS(wifiActuallyEnabled ? 5 : 100));
    }
}

void webPortalInit() { xTaskCreatePinnedToCore(webTaskFn, "webTask", 12288, NULL, 1, NULL, 0); }
