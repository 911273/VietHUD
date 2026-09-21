#include "WebPortal.h"
#include "core/AppConfig.h"
#include "core/NvsStore.h"
#include "core/SharedState.h"
#include "map/SdCardManager.h" // sdMgrListTripLogs()/sdMgrReadFileChunk() — /triplog download page
#include "map/SpeedLimitManager.h" // speedLimitManagerGetInfo()/speedSourceStr() — /api/speedlimit, /api/speedmap/debug
#include "ui/Dashboard.h" // applyConfig() — a web-submitted brightness change needs the same PWM rewrite Settings.cpp's slider does
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h> // isinf() — used directly below, not just transitively via AppConfig.h
#include <string.h> // strlen() — used in applyWifiState()'s password-length check

// AP mode, not STA: this rides in a vehicle, so "join the existing network"
// isn't a stable concept the way it is for a fixed installation — the
// device brings its own hotspot instead, same reasoning as GNSS/radar being
// wired directly rather than depending on anything external. SSID/password
// now live in AppConfig (cfg.wifiSsid/cfg.wifiPassword — editable from
// Settings > WiFi or /config), not hardcoded here.
static WebServer server(80);

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

void webPortalStatusText(char *buf, size_t cap) {
    if (wifiActuallyEnabled) snprintf(buf, cap, "ON, IP=%s", WiFi.softAPIP().toString().c_str());
    else snprintf(buf, cap, "OFF");
}

static const char *relationStr(Relation r) {
    switch (r) {
        case SAME_LANE: return "SAME_LANE";
        case ADJACENT_LEFT: return "ADJACENT_LEFT";
        case ADJACENT_RIGHT: return "ADJACENT_RIGHT";
        case OPPOSITE: return "OPPOSITE";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------
// "/" — live telemetry page. Static HTML/CSS/JS (PROGMEM, no per-request
// formatting needed) that polls /api/status every 500ms and fills in the
// DOM — same cadence Settings.cpp's own refreshSensorsPanel() timer uses
// for the same reasoning: live values only need to be as fresh as a human
// reads them, no need for anything tighter.
// ---------------------------------------------------------------------
static const char kIndexHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Radar Car - Live</title>
<style>
body{background:#0B0F14;color:#CCD6E0;font-family:sans-serif;margin:0;padding:12px}
h1{font-size:18px;color:#fff;margin:0 0 12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}
.card{background:#151C24;border-radius:8px;padding:10px}
.card .label{color:#7C8A9A;font-size:11px;text-transform:uppercase}
.card .value{color:#fff;font-size:20px;margin-top:4px}
.ok{color:#33CC66}.warn{color:#E0C020}.bad{color:#FF3B30}
.card.camera{background:#E0A020;grid-column:1/-1}
.card.camera .label,.card.camera .value{color:#000}
nav a{color:#4AA3FF;margin-right:16px;font-size:13px;text-decoration:none}
</style></head><body>
<nav><a href="/">Live</a><a href="/triplog">Trip logs</a><a href="/config">Config</a><a href="/update">OTA Update</a></nav>
<h1>Radar Car - Live Telemetry</h1>
<div class="grid" id="grid"></div>
<script>
function card(label, value, cls) {
  return '<div class="card"><div class="label">' + label + '</div><div class="value ' + (cls||'') + '">' + value + '</div></div>';
}
async function tick() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    let html = '';
    // Speed-camera-ahead — full-width amber card, same warning color/meaning
    // as the on-device Dashboard's own banner (ui/Dashboard.cpp's
    // cameraAheadLabel: a known, upcoming hazard, not a live collision risk,
    // hence its own color rather than reusing .ok/.warn/.bad above). Only
    // rendered while a camera is actually ahead, not as an always-present
    // "no camera" card — matches the on-device banner's own hide/show logic.
    if (d.cameraAhead) {
      const c = d.cameraAhead;
      const limitTxt = (c.speedLimitKmh !== null) ? ' (' + c.speedLimitKmh.toFixed(0) + ' km/h)' : '';
      html += '<div class="card camera"><div class="label">Speed camera ahead</div><div class="value">' +
              c.distanceM.toFixed(0) + ' m' + limitTxt + '</div></div>';
    }
    html += card('Radar link', d.radar.online ? 'OK' : 'FAULT', d.radar.online ? 'ok' : 'bad');
    html += card('Frames / errors', d.radar.framesParsed + ' / ' + d.radar.parseErrors);
    html += card('Active targets', d.radar.activeTargets);
    html += card('Last qty / alarm / snr', d.radar.lastQty + ' / ' + d.radar.lastAlarm + ' / ' + d.radar.lastSnr);
    if (d.radar.primary) {
      const p = d.radar.primary;
      html += card('Primary distance', p.distanceM.toFixed(1) + ' m');
      html += card('Primary TTC', isFinite(p.ttcS) ? p.ttcS.toFixed(1) + ' s' : '--');
      html += card('Primary relation', p.relation);
      html += card('Primary SNR', p.snr);
    }
    html += card('GNSS fix', d.gnss.fix ? 'OK' : (d.gnss.linkAlive ? 'SEARCHING' : 'FAULT'),
                  d.gnss.fix ? 'ok' : (d.gnss.linkAlive ? 'warn' : 'bad'));
    html += card('Satellites', d.gnss.satCount);
    html += card('Speed (filtered)', d.gnss.speedKmh.toFixed(1) + ' km/h');
    html += card('Speed (raw)', d.gnss.rawSpeedKmh.toFixed(1) + ' km/h');
    html += card('Free internal RAM', d.mem.freeInternalKB + ' KB (min ' + d.mem.minFreeInternalKBEver + ' KB)');
    html += card('Free PSRAM', d.mem.freePsramKB + ' KB');
    html += card('Uptime', Math.floor(d.uptimeMs / 1000) + ' s');
    document.getElementById('grid').innerHTML = html;
  } catch (e) { /* transient fetch failure — next tick retries */ }
}
tick();
setInterval(tick, 500);
</script></body></html>)HTML";

static void handleIndex() { server.send_P(200, "text/html", kIndexHtml); }

// ---------------------------------------------------------------------
// "/api/status" — hand-rolled JSON (no ArduinoJson dependency — same
// reasoning as everywhere else in this project that a small, fixed set of
// fields is simpler formatted directly than pulling in a library for it).
// Static buffer, not stack-local: webTaskFn is the only task that ever
// calls into this server, so there's no concurrent-write hazard, and a
// ~1.5KB buffer is safer off a FreeRTOS task's stack than on it.
// ---------------------------------------------------------------------
// Bumped 1536->1792 (2026-09-21) alongside adding the "camera" object below
// — same "comfortable margin over measured worst case" convention as
// radarTaskStart()'s stack sizing, not a tight fit.
static char statusBuf[1792];

static void handleApiStatus() {
    RadarSnapshot radar = radarSnapshot();
    GnssSnapshot gnss = gnssSnapshot();
    RoadInfoSnapshot road = roadInfoSnapshot();

    int activeCount = 0;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (radar.targets[i].active) activeCount++;

    char primaryBuf[192] = "null";
    if (radar.primaryIdx >= 0) {
        SimTarget &p = radar.targets[radar.primaryIdx];
        snprintf(primaryBuf, sizeof(primaryBuf),
                 "{\"idx\":%d,\"distanceM\":%.1f,\"ttcS\":%s,\"speedKmh\":%.1f,\"angleDeg\":%.0f,"
                 "\"relation\":\"%s\",\"confidence\":%.2f,\"snr\":%.0f,\"tooClose\":%s}",
                 radar.primaryIdx, (double)p.distanceM,
                 isinf(p.ttcS) ? "null" : String(p.ttcS, 1).c_str(), (double)(p.closingSpeedMps * 3.6f),
                 (double)p.angleDeg, relationStr(p.relation), (double)p.confidence, (double)p.snr,
                 p.tooClose ? "true" : "false");
    }

    // Speed-camera-ahead (user-requested 2026-09-21, "them the hien phia
    // truoc co camera") — same map/SpeedLimitManager.cpp's matchCameraAhead()
    // result the Dashboard's own amber banner already shows on-device (see
    // ui/Dashboard.cpp's cameraAheadLabel); this is the same data, just
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

    snprintf(statusBuf, sizeof(statusBuf),
             "{\"uptimeMs\":%lu,"
             "\"radar\":{\"online\":%s,\"framesParsed\":%lu,\"parseErrors\":%lu,\"lastQty\":%u,\"lastAlarm\":%u,"
             "\"lastSnr\":%u,\"audioAllowed\":%s,\"harshBrakeWarning\":%s,\"activeTargets\":%d,\"primary\":%s},"
             "\"gnss\":{\"fix\":%s,\"linkAlive\":%s,\"satCount\":%d,\"speedKmh\":%.1f,\"rawSpeedKmh\":%.1f,"
             "\"timeValid\":%s,\"utcHour\":%d,\"utcMinute\":%d},"
             "\"cameraAhead\":%s,"
             "\"mem\":{\"freeInternalKB\":%u,\"minFreeInternalKBEver\":%u,\"freePsramKB\":%u}}",
             (unsigned long)millis(), radar.online ? "true" : "false", (unsigned long)radar.framesParsed,
             (unsigned long)radar.parseErrors, radar.lastTargetQty, radar.lastAlarm, radar.lastSnr,
             radar.audioAllowed ? "true" : "false", radar.harshBrakeWarning ? "true" : "false", activeCount,
             primaryBuf, gnss.fix ? "true" : "false",
             gnss.linkAlive ? "true" : "false", gnss.satCount, (double)gnss.egoSpeedKmh, (double)gnss.rawSpeedKmh,
             gnss.timeValid ? "true" : "false", gnss.utcHour, gnss.utcMinute, cameraBuf,
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
    // (the radar/GNSS/map tasks all share that same card).
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
static char configBuf[6144];

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

// 3-way labeled picker for cfg.radarDirection — mirrors ui/Settings.cpp's
// addChoiceRow() switch from a raw 0/1/2 number field to labeled options
// (matching the vendor's own BLE config app's "Stay away and approach"
// wording, not a bare number). Option values are the LD2451's own byte
// encoding (0=away,1=approach,2=both — see radar/LD2451.cpp's configureRadar()),
// so handleConfigPost()'s existing argFloat("radarDirection", ...) needs no
// change: a <select> submits the same numeric string a <input type=number>
// would have.
static size_t appendDirectionSelect(char *buf, size_t cap, size_t len, float value) {
    static const char *kLabels[3] = {"Away only", "Approach only", "Both (away + approach)"};
    len += snprintf(buf + len, len < cap ? cap - len : 0, "<label>Direction<select name=\"radarDirection\">");
    for (int i = 0; i < 3; i++) {
        len += snprintf(buf + len, len < cap ? cap - len : 0, "<option value=\"%d\"%s>%s</option>", i,
                         ((int)(value + 0.5f) == i) ? " selected" : "", kLabels[i]);
    }
    len += snprintf(buf + len, len < cap ? cap - len : 0, "</select></label>");
    return len;
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
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Radar Car - "
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
                     "<nav><a href=\"/\">Live</a><a href=\"/config\">Config</a><a href=\"/update\">OTA "
                     "Update</a></nav>"
                     "<h1>Radar Car - Configuration</h1><form method=\"POST\" action=\"/config\">");

    len += snprintf(configBuf + len, sizeof(configBuf) - len, "<fieldset><legend>Radar</legend>");
    len = appendField(configBuf, sizeof(configBuf), len, "maxRangeM", "Max range (m)", cfg.maxRangeM, 1, 10, 100);
    len = appendField(configBuf, sizeof(configBuf), len, "minTargetSpeedKmh", "Min target speed (km/h)",
                       cfg.minTargetSpeedKmh, 1, 0, 30);
    len = appendField(configBuf, sizeof(configBuf), len, "maxTargets", "Max targets", cfg.maxTargets, 1, 1, 5);
    len = appendDirectionSelect(configBuf, sizeof(configBuf), len, cfg.radarDirection);
    len = appendField(configBuf, sizeof(configBuf), len, "radarSnrLevel", "SNR sensitivity", cfg.radarSnrLevel, 1, 0,
                       8);
    len = appendField(configBuf, sizeof(configBuf), len, "radarTriggerCount", "Trigger count",
                       cfg.radarTriggerCount, 1, 1, 10);
    len = appendField(configBuf, sizeof(configBuf), len, "radarNoTargetDelayS", "No-target delay (s)",
                       cfg.radarNoTargetDelayS, 1, 0, 30);
    len = appendField(configBuf, sizeof(configBuf), len, "laneHalfWidthM", "Lane half-width (m)",
                       cfg.laneHalfWidthM, 0.1f, 0.5f, 5);
    len += snprintf(configBuf + len, sizeof(configBuf) - len, "</fieldset><fieldset><legend>Safety</legend>");
    len = appendField(configBuf, sizeof(configBuf), len, "audioEnableKmh", "Audio enable speed (km/h)",
                       cfg.audioEnableKmh, 1, 40, 120);
    len = appendField(configBuf, sizeof(configBuf), len, "hysteresisKmh", "Hysteresis (km/h)", cfg.hysteresisKmh, 1,
                       1, 10);
    len = appendField(configBuf, sizeof(configBuf), len, "ttcWarnS", "TTC warning (s)", cfg.ttcWarnS, 0.1f, 1, 10);
    len = appendField(configBuf, sizeof(configBuf), len, "ttcCritS", "TTC critical (s)", cfg.ttcCritS, 0.1f, 0.5f,
                       5);
    len = appendField(configBuf, sizeof(configBuf), len, "minConfidence", "Min confidence (0-1)", cfg.minConfidence,
                       0.01f, 0, 1);
    len = appendField(configBuf, sizeof(configBuf), len, "minDistanceM", "Min distance (m)", cfg.minDistanceM, 0.5f,
                       1, 20);
    len = appendField(configBuf, sizeof(configBuf), len, "harshBrakeAccelMps2", "Harsh brake sensitivity (m/s2)",
                       cfg.harshBrakeAccelMps2, 0.5f, 1, 10);
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "audioEnabled", "Audio enabled", cfg.audioEnabled);
    len += snprintf(configBuf + len, sizeof(configBuf) - len, "</fieldset><fieldset><legend>Display</legend>");
    len = appendField(configBuf, sizeof(configBuf), len, "brightness", "Brightness (%)", cfg.brightness, 1, 5, 100);
    len = appendField(configBuf, sizeof(configBuf), len, "autoDimMin", "Auto-dim after (min)", cfg.autoDimMin, 1, 0,
                       30);
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "showId", "Show target ID", cfg.showId);
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "showSpeed", "Show target speed", cfg.showSpeed);
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "showTtc", "Show target TTC", cfg.showTtc);
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "showAngle", "Show target angle", cfg.showAngle);
    len += snprintf(configBuf + len, sizeof(configBuf) - len, "</fieldset><fieldset><legend>Sensors (GNSS)</legend>");
    len = appendField(configBuf, sizeof(configBuf), len, "gnssSpeedFilterAlpha", "Speed filter smoothing",
                       cfg.gnssSpeedFilterAlpha, 0.01f, 0.05f, 0.90f);
    len = appendField(configBuf, sizeof(configBuf), len, "gnssFixTimeoutS", "Fix timeout (s)", cfg.gnssFixTimeoutS,
                       0.1f, 1, 10);
    len = appendCheckbox(configBuf, sizeof(configBuf), len, "tripLoggingEnabled", "Trip logging (SD card)",
                          cfg.tripLoggingEnabled);
    len += snprintf(configBuf + len, sizeof(configBuf) - len, "</fieldset><fieldset><legend>WiFi</legend>");
    len = appendTextField(configBuf, sizeof(configBuf), len, "wifiSsid", "SSID", cfg.wifiSsid, false);
    len = appendTextField(configBuf, sizeof(configBuf), len, "wifiPassword", "New password (blank = keep current)",
                           "", true);
    len += snprintf(configBuf + len, sizeof(configBuf) - len,
                     "<p style=\"font-size:11px;color:#7C8A9A\">On/off is on the device screen only (Settings &gt; "
                     "WiFi, or hold the Dashboard 3s+) — not here, since submitting an off request over WiFi would "
                     "disconnect this page mid-request.</p>"
                     "</fieldset><button type=\"submit\">Save to device</button></form></body></html>");

    server.send(200, "text/html", configBuf);
}

static float argFloat(const char *name, float fallback) {
    if (!server.hasArg(name)) return fallback;
    return server.arg(name).toFloat();
}

static void handleConfigPost() {
    cfg.maxRangeM = argFloat("maxRangeM", cfg.maxRangeM);
    cfg.minTargetSpeedKmh = argFloat("minTargetSpeedKmh", cfg.minTargetSpeedKmh);
    cfg.maxTargets = argFloat("maxTargets", cfg.maxTargets);
    cfg.radarDirection = argFloat("radarDirection", cfg.radarDirection);
    cfg.radarSnrLevel = argFloat("radarSnrLevel", cfg.radarSnrLevel);
    cfg.radarTriggerCount = argFloat("radarTriggerCount", cfg.radarTriggerCount);
    cfg.radarNoTargetDelayS = argFloat("radarNoTargetDelayS", cfg.radarNoTargetDelayS);
    cfg.laneHalfWidthM = argFloat("laneHalfWidthM", cfg.laneHalfWidthM);
    cfg.audioEnableKmh = argFloat("audioEnableKmh", cfg.audioEnableKmh);
    cfg.hysteresisKmh = argFloat("hysteresisKmh", cfg.hysteresisKmh);
    cfg.ttcWarnS = argFloat("ttcWarnS", cfg.ttcWarnS);
    cfg.ttcCritS = argFloat("ttcCritS", cfg.ttcCritS);
    cfg.minConfidence = argFloat("minConfidence", cfg.minConfidence);
    cfg.minDistanceM = argFloat("minDistanceM", cfg.minDistanceM);
    cfg.harshBrakeAccelMps2 = argFloat("harshBrakeAccelMps2", cfg.harshBrakeAccelMps2);
    cfg.brightness = argFloat("brightness", cfg.brightness);
    cfg.autoDimMin = argFloat("autoDimMin", cfg.autoDimMin);
    cfg.gnssSpeedFilterAlpha = argFloat("gnssSpeedFilterAlpha", cfg.gnssSpeedFilterAlpha);
    cfg.gnssFixTimeoutS = argFloat("gnssFixTimeoutS", cfg.gnssFixTimeoutS);
    if (server.hasArg("wifiSsid")) {
        strncpy(cfg.wifiSsid, server.arg("wifiSsid").c_str(), sizeof(cfg.wifiSsid) - 1);
        cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0';
    }
    // Blank submitted password means "leave it unchanged" — see appendTextField()'s comment.
    if (server.hasArg("wifiPassword") && server.arg("wifiPassword").length() > 0) {
        strncpy(cfg.wifiPassword, server.arg("wifiPassword").c_str(), sizeof(cfg.wifiPassword) - 1);
        cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0';
    }
    // Checkboxes only appear in POST data when checked — an absent arg means unchecked, not "leave unchanged".
    cfg.audioEnabled = server.hasArg("audioEnabled");
    cfg.showId = server.hasArg("showId");
    cfg.showSpeed = server.hasArg("showSpeed");
    cfg.showTtc = server.hasArg("showTtc");
    cfg.showAngle = server.hasArg("showAngle");
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
<title>Radar Car - OTA Update</title>
<style>body{background:#0B0F14;color:#CCD6E0;font-family:sans-serif;margin:0;padding:12px}
h1{font-size:18px;color:#fff}nav a{color:#4AA3FF;margin-right:16px;font-size:13px;text-decoration:none}
button{background:#2E7D4F;color:#fff;border:0;border-radius:6px;padding:10px 20px;font-size:14px}
p.warn{color:#E0C020}</style></head><body>
<nav><a href="/">Live</a><a href="/triplog">Trip logs</a><a href="/config">Config</a><a href="/update">OTA Update</a></nav>
<h1>Radar Car - OTA Firmware Update</h1>
<p class="warn">Upload a .bin built for env:uidemo. Do not power off during upload — the device reboots automatically when done.</p>
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
// Applies a WiFi on/off transition — the ONLY place in this file that calls
// WiFi.*()/server.begin()/server.stop(), and only ever from webTaskFn()'s
// own context (see wifiEnabledRequest's comment above for why that
// matters). A same-target call (already on, asked for on again) is a no-op.
static void applyWifiState(bool enable) {
    if (enable == wifiActuallyEnabled) return;
    if (enable) {
        WiFi.mode(WIFI_AP);
        const char *ssid = cfg.wifiSsid[0] ? cfg.wifiSsid : "RadarCar"; // guard an emptied-out SSID field
        size_t pwLen = strlen(cfg.wifiPassword);
        bool secured = pwLen >= 8; // WPA2 minimum — WiFi.softAP() silently fails to secure below this
        bool ok = secured ? WiFi.softAP(ssid, cfg.wifiPassword) : WiFi.softAP(ssid);
        if (!secured) {
            Serial.printf("[web] WARN: password %s (%u chars, WPA2 needs >=8) — starting an OPEN (unsecured) AP\n",
                          pwLen == 0 ? "empty" : "too short", (unsigned)pwLen);
        }
        server.begin();
        Serial.printf("[web] WiFi ON — AP \"%s\" (%s) %s, IP=%s\n", ssid, secured ? "secured" : "OPEN",
                      ok ? "up" : "FAILED to start", WiFi.softAPIP().toString().c_str());
    } else {
        server.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
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

    esp_task_wdt_add(NULL);
    for (;;) {
        if (wifiEnabledRequest != wifiActuallyEnabled) applyWifiState(wifiEnabledRequest);
        if (wifiActuallyEnabled) server.handleClient();
        esp_task_wdt_reset();
        // No need to poll fast while off — nothing to service until someone
        // asks for WiFi again.
        vTaskDelay(pdMS_TO_TICKS(wifiActuallyEnabled ? 5 : 100));
    }
}

void webPortalInit() { xTaskCreatePinnedToCore(webTaskFn, "webTask", 8192, NULL, 1, NULL, 0); }
