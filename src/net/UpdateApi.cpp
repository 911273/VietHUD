#include "UpdateApi.h"
#include "WebPortal.h"
#include "DataUpdater.h"
#include "core/AppConfig.h"
#include "core/SharedState.h"
#include "core/Version.h"
#include "update/DataInstaller.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_task_wdt.h>

static PortalServer *srv = nullptr;
static uint32_t sLastActivityMs = 0;
static uint32_t sRebootAtMs = 0;
static uint32_t sDirectAtMs = 0;

// Raw-upload state for the request currently streaming in.
static int sPutCode = 0;
static uint32_t sPutExpected = 0;

static void touch() { sLastActivityMs = millis() | 1; }

bool updateApiBusy() {
    if (sLastActivityMs && millis() - sLastActivityMs < 120000) return true;
    InstallerProgress p = installerProgress();
    return p.state == INST_VERIFYING || p.state == INST_COMMITTED;
}

// Every state-changing call must carry "X-VietHUD: 1". A custom header forces a
// CORS preflight for any cross-origin page, which this server never answers —
// so a random website the phone happens to have open can't drive the updater.
static bool csrfOk() {
    if (srv->header("X-VietHUD") == "1") return true;
    srv->send(403, "application/json", "{\"error\":403,\"detail\":\"missing X-VietHUD header\"}");
    return false;
}

static void sendErr(int code, const char *detail) {
    String o = "{\"error\":";
    o += code;
    o += ",\"detail\":\"";
    o += detail;
    o += "\"}";
    srv->send(code, "application/json", o);
}

static void deviceId(char *out, size_t cap) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(out, cap, "%02X%02X", mac[4], mac[5]);
}

static void handlePing() {
    char id[8], b[96];
    deviceId(id, sizeof(id));
    snprintf(b, sizeof(b), "{\"ok\":true,\"id\":\"%s\",\"fw\":\"%s\",\"up\":%lu}", id, VIETHUD_FW_VERSION,
             (unsigned long)millis());
    srv->sendHeader("Cache-Control", "no-store");
    srv->send(200, "application/json", b);
}

static void handleState() {
    char id[8], ap[40], ip[24] = "";
    deviceId(id, sizeof(id));
    webPortalApSsid(ap, sizeof(ap));
    bool sta = webPortalStaIp(ip, sizeof(ip));
    GnssSnapshot g = gnssSnapshot();
    String o;
    o.reserve(2600);
    char b[320];
    snprintf(b, sizeof(b),
             "{\"device\":{\"id\":\"%s\",\"fw\":\"%s\",\"ap\":\"%s\",\"moving\":%s,\"up\":%lu},"
             "\"sta\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"saved\":%d},\"url\":\"%s\",\"data\":",
             id, VIETHUD_FW_VERSION, ap, (g.fix && g.egoSpeedKmh > 8.0f) ? "true" : "false", (unsigned long)millis(),
             sta ? "true" : "false", sta ? cfg.staSsid : "", ip, cfg.savedNetworkCount, cfg.dataUpdateUrl);
    o += b;
    installerStateJson(o);
    o += "}";
    srv->sendHeader("Cache-Control", "no-store");
    srv->send(200, "application/json", o);
}

// Body (text/plain): "sig <base64>\n" + the exact manifest.txt bytes.
static void handleSessionPost() {
    if (!csrfOk()) return;
    touch();
    String body = srv->arg("plain");
    int nl = body.indexOf('\n');
    if (!body.startsWith("sig ") || nl < 0) return sendErr(400, "Yêu cầu không hợp lệ");
    String sig = body.substring(4, nl);
    const char *manifest = body.c_str() + nl + 1;
    size_t mlen = body.length() - nl - 1;
    char err[96] = "";
    int code = installerOpenSession(manifest, mlen, sig.c_str(), err, sizeof(err));
    if (code == 200) {
        srv->send(200, "application/json", "{\"ok\":true,\"upToDate\":true,\"session\":null}");
        return;
    }
    if (code != 201) return sendErr(code, err);
    String o = "{\"ok\":true,\"upToDate\":false,\"chunkMax\":262144,\"chunkAlign\":1436,\"session\":";
    installerSessionJson(o);
    o += "}";
    srv->send(201, "application/json", o);
}

static void handleSessionGet() {
    String o = "{\"session\":";
    installerSessionJson(o);
    InstallerProgress p = installerProgress();
    o += ",\"msg\":\"";
    o += p.message;
    o += "\"}";
    srv->sendHeader("Cache-Control", "no-store");
    srv->send(200, "application/json", o);
}

static void handleSessionDelete() {
    if (!csrfOk()) return;
    installerAbort();
    srv->send(200, "application/json", "{\"ok\":true}");
}

// PUT /api/v1/update/file — headers X-Sid / X-Name / X-Offset (the raw-body
// path of WebServer does NOT parse URL query args), body = raw bytes that go
// straight to the SD card 1436 B at a time — nothing is buffered in RAM.
static void handleFileRaw() {
    HTTPRaw &raw = srv->raw();
    if (raw.status == RAW_START) {
        touch();
        sPutExpected = 0;
        // WebServer pulls the body with readBytes(buf, 1436), which blocks until
        // 1436 bytes arrive OR the stream timeout (5 s by default) expires. The
        // tail of every body is shorter than that, so each request paid a fixed
        // ~5 s (measured: 64 KB took 5.4 s). Clients send 1436-multiple chunks,
        // and the one short tail per file now costs at most this much.
        srv->setCurrentReadTimeoutMs(800);
        if (srv->header("X-VietHUD") != "1") {
            sPutCode = 403;
            return;
        }
        if (srv->header("X-Sid") != installerSessionId() || !installerSessionActive()) {
            sPutCode = 410;
            return;
        }
        String name = srv->header("X-Name");
        uint32_t off = (uint32_t)strtoul(srv->header("X-Offset").c_str(), nullptr, 10);
        sPutCode = installerWriteBegin(name.c_str(), off, &sPutExpected);
    } else if (raw.status == RAW_WRITE) {
        if (sPutCode == 200 && !installerWriteData(raw.buf, raw.currentSize)) sPutCode = 413;
        esp_task_wdt_reset(); // a 256 KB chunk on weak WiFi can outlast the 10 s task watchdog
        touch();
    } else if (raw.status == RAW_END || raw.status == RAW_ABORTED) {
        // Close either way: whatever reached the card is kept, and the client
        // resumes from the part's real size (GET session / 409 expected).
        if (sPutCode == 200 || sPutCode == 413) sPutExpected = installerWriteEnd();
    }
}

static void handleFileDone() {
    char b[96];
    if (sPutCode == 200) {
        snprintf(b, sizeof(b), "{\"ok\":true,\"received\":%u}", (unsigned)sPutExpected);
        srv->send(200, "application/json", b);
    } else if (sPutCode == 409 || sPutCode == 413) {
        snprintf(b, sizeof(b), "{\"error\":%d,\"expected\":%u}", sPutCode, (unsigned)sPutExpected);
        srv->send(409, "application/json", b);
    } else {
        sendErr(sPutCode ? sPutCode : 400,
                sPutCode == 410 ? "Phiên cập nhật đã hết" : sPutCode == 404 ? "Tệp không có trong phiên" : "Lỗi ghi");
    }
    sPutCode = 0;
}

static void handleCommit() {
    if (!csrfOk()) return;
    touch();
    if (srv->header("X-Sid") != installerSessionId()) return sendErr(410, "Phiên cập nhật đã hết");
    char err[96] = "";
    int code = installerCommit(true, err, sizeof(err));
    if (code != 200) return sendErr(code, err);
    srv->sendHeader("Connection", "close");
    srv->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    sRebootAtMs = millis() + 1500; // let the reply reach the phone first
}

static void handleDirect() {
    if (!csrfOk()) return;
    if (cfg.dataUpdateUrl[0] == '\0') return sendErr(424, "Chưa đặt địa chỉ dữ liệu");
    if (cfg.savedNetworkCount == 0 && cfg.staSsid[0] == '\0') return sendErr(424, "VietHUD chưa có Wi-Fi Internet");
    srv->sendHeader("Connection", "close");
    srv->send(202, "application/json", "{\"ok\":true,\"reboot\":true}");
    sDirectAtMs = millis() + 1200;
}

void updateApiRegister(PortalServer &s) {
    srv = &s;
    static const char *kHeaders[] = {"X-VietHUD", "X-Sid", "X-Name", "X-Offset"};
    s.collectHeaders(kHeaders, 4);
    s.on("/api/v1/ping", HTTP_GET, handlePing);
    s.on("/api/v1/update/state", HTTP_GET, handleState);
    s.on("/api/v1/update/session", HTTP_POST, handleSessionPost);
    s.on("/api/v1/update/session", HTTP_GET, handleSessionGet);
    s.on("/api/v1/update/session", HTTP_DELETE, handleSessionDelete);
    s.on("/api/v1/update/file", HTTP_PUT, handleFileDone, handleFileRaw);
    s.on("/api/v1/update/commit", HTTP_POST, handleCommit);
    s.on("/api/v1/update/direct", HTTP_POST, handleDirect);
}

void updateApiLoop() {
    if (sRebootAtMs && (int32_t)(millis() - sRebootAtMs) >= 0) {
        Serial.println("[update] rebooting to apply staged data");
        delay(100);
        ESP.restart();
    }
    if (sDirectAtMs && (int32_t)(millis() - sDirectAtMs) >= 0) {
        sDirectAtMs = 0;
        dataUpdateSchedule(); // NVS flag + reboot into update mode (never returns)
    }
}
