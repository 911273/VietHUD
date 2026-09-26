#include "DataUpdater.h"
#include "core/AppConfig.h"
#include "map/SdCardManager.h"
#include "update/DataInstaller.h" // staging + signature + atomic boot-time install
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/sha256.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// The core (vector) data set the updater manages. maptiles*.bin is deliberately
// NOT here (vector-only). A file listed in the remote manifest but not here is
// still downloaded — this array only bounds the local-manifest parse.
#define DU_MAX_FILES 24
#define DU_SPEEDMAP_DIR "/speedmap/"
#define DU_MANIFEST_NAME "manifest.txt"

static DataUpdateStatus s_status = {DU_IDLE, "", 0, 0, 0};

// Auto-check state (2026-09-26): a lightweight "is a newer version published?"
// probe that only fetches manifest.txt and compares its version line to the copy
// on the SD card. Sets a flag the UI shows; it never downloads or reboots.
static volatile bool s_checkRunning = false;
static volatile bool s_updateAvailable = false;
static char s_remoteVersion[24] = "";
static char s_localVersion[24] = "";
static volatile bool s_running = false;

DataUpdateStatus dataUpdateGetStatus() { return s_status; }

static void setStatus(DataUpdateState st, const char *msg) {
    s_status.state = st;
    strncpy(s_status.message, msg ? msg : "", sizeof(s_status.message) - 1);
    s_status.message[sizeof(s_status.message) - 1] = '\0';
    Serial.printf("[dataupd] %s\n", s_status.message);
}

// HTTP(S) GET into a String (small responses only — manifest + signature).
// Returns true on 200. Uses an insecure TLS client for https (no cert store
// on-device) — acceptable because the manifest is ECDSA-signed and every file
// is SHA-256 checked against it (a MITM can only cause a failed update).
static bool httpGetText(const String &url, String &out) {
    Serial.printf("[dataupd] heap before TLS: free=%u biggest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    WiFiClientSecure sclient;
    WiFiClient client;
    bool https = url.startsWith("https");
    if (https) sclient.setInsecure();
    HTTPClient http;
    if (!http.begin(https ? (WiFiClient &)sclient : client, url)) return false;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // GitHub raw 301/302s to the Fastly CDN
    http.setTimeout(15000);
    int code = http.GET();
    bool ok = (code == 200);
    if (ok) out = http.getString();
    else
        Serial.printf("[dataupd] GET %s -> %d (%s)\n", url.c_str(), code,
                      code < 0 ? HTTPClient::errorToString(code).c_str() : "http-status");
    http.end();
    return ok;
}

// Streams one file into the installer's staging part (/vhupd/<name>.part),
// resuming from whatever is already on the card via an HTTP Range request.
// Verification happens later, in installerCommit() (readback SHA-256).
static bool downloadToStage(const String &base, int idx, uint8_t *buf, size_t bufSize) {
    const char *name;
    uint32_t size = 0, have = 0;
    if (!installerFileInfo(idx, &name, &size, &have, nullptr)) return false;
    if (have >= size) return true;

    WiFiClientSecure sclient;
    WiFiClient client;
    String url = base + name;
    bool https = url.startsWith("https");
    if (https) sclient.setInsecure();
    HTTPClient http;
    if (!http.begin(https ? (WiFiClient &)sclient : client, url)) {
        setStatus(DU_FAILED, "Lỗi kết nối máy chủ");
        return false;
    }
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    if (have > 0) http.addHeader("Range", String("bytes=") + have + "-");
    int code = http.GET();
    if (code != 200 && code != 206) {
        char m[64];
        snprintf(m, sizeof(m), "Lỗi tải %s (HTTP %d)", name, code);
        setStatus(DU_FAILED, m);
        http.end();
        return false;
    }
    uint32_t skip = (code == 200) ? have : 0; // server ignored Range: discard what we already have
    uint32_t expected = 0;
    if (installerWriteBegin(name, have, &expected) != 200) {
        http.end();
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    uint32_t got = have;
    uint32_t lastDataMs = millis();
    bool ok = true;
    while (got < size && http.connected()) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastDataMs > 15000) {
                setStatus(DU_FAILED, "Mất kết nối khi đang tải");
                ok = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        int r = stream->readBytes(buf, avail > bufSize ? bufSize : avail);
        if (r <= 0) continue;
        lastDataMs = millis();
        const uint8_t *p = buf;
        if (skip) {
            uint32_t s = (uint32_t)r < skip ? (uint32_t)r : skip;
            skip -= s;
            p += s;
            r -= s;
            if (r == 0) continue;
        }
        if ((uint32_t)r > size - got) r = size - got;
        if (!installerWriteData(p, r)) {
            setStatus(DU_FAILED, "Lỗi ghi thẻ nhớ");
            ok = false;
            break;
        }
        got += r;
        s_status.percent = (int)((int64_t)got * 100 / (size ? size : 1));
    }
    http.end();
    uint32_t now = installerWriteEnd();
    return ok && now >= size;
}

// Returns: 0 = failed, 1 = success WITH changes staged (caller reboots so the
// boot-time installer swaps them in), 2 = success, nothing to update.
static int runUpdate(uint8_t *dlBuf, size_t dlBufSize) {
    s_status.filesTotal = 0;
    s_status.filesDone = 0;
    s_status.percent = 0;
    setStatus(DU_RUNNING, "Đang tải danh sách dữ liệu...");

    String base = cfg.dataUpdateUrl;
    if (base.length() && !base.endsWith("/")) base += "/";
    Serial.printf("[dataupd] STA ip=%s dns=%s  url=%s\n", WiFi.localIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(), (base + DU_MANIFEST_NAME).c_str());

    String manifest, sig;
    if (!httpGetText(base + DU_MANIFEST_NAME, manifest)) {
        setStatus(DU_FAILED, "Không tải được danh sách dữ liệu");
        return 0;
    }
    if (!httpGetText(base + DU_MANIFEST_NAME ".sig", sig)) {
        setStatus(DU_FAILED, "Bản cập nhật chưa được ký");
        return 0;
    }
    char err[80];
    int rc = installerOpenSession(manifest.c_str(), manifest.length(), sig.c_str(), err, sizeof(err));
    if (rc == 200) {
        setStatus(DU_SUCCESS, "Dữ liệu đã là bản mới nhất");
        return 2;
    }
    if (rc != 201) {
        setStatus(DU_FAILED, err);
        return 0;
    }
    int n = installerFileCount();
    s_status.filesTotal = n;
    for (int k = 0; k < n; k++) {
        const char *name = "";
        installerFileInfo(k, &name, nullptr, nullptr, nullptr);
        s_status.filesDone = k;
        s_status.percent = 0;
        char m[64];
        snprintf(m, sizeof(m), "Đang tải %s (%d/%d)", name, k + 1, n);
        setStatus(DU_RUNNING, m);
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) { // resumes via Range after a drop
            if (attempt) vTaskDelay(pdMS_TO_TICKS(2000));
            ok = downloadToStage(base, k, dlBuf, dlBufSize);
        }
        if (!ok) return 0; // FAILED set inside; parts stay on the card for the next try
        s_status.filesDone = k + 1;
    }
    setStatus(DU_RUNNING, "Đang kiểm tra dữ liệu...");
    rc = installerCommit(false, err, sizeof(err));
    if (rc != 200) {
        setStatus(DU_FAILED, err);
        return 0;
    }
    char done[64];
    snprintf(done, sizeof(done), "Đã tải %d tệp — khởi động lại để cài", n);
    setStatus(DU_SUCCESS, done);
    return 1;
}

static void dataUpdateTask(void *) {
    // Download buffer in PSRAM so internal RAM stays free for WiFi/TLS.
    const size_t DL = 4096;
    uint8_t *dlBuf = (uint8_t *)heap_caps_malloc(DL, MALLOC_CAP_SPIRAM);
    int result = 0;
    if (dlBuf) result = runUpdate(dlBuf, DL);
    else setStatus(DU_FAILED, "Hết bộ nhớ");
    if (dlBuf) heap_caps_free(dlBuf);

    s_running = false;
    if (result == 1) {
        vTaskDelay(pdMS_TO_TICKS(2500)); // let the UI show success + the HTTP reply flush
        ESP.restart();
    }
    vTaskDelete(NULL);
}

// Schedule an update: set an NVS flag and reboot. The actual download runs in
// "update mode" at the next boot (main_viethud.cpp), where only display+WiFi+SD
// are up — so the TLS handshake has the large contiguous RAM it needs, which it
// never has while the full app (map/audio/GNSS tasks + AP) is running.
void dataUpdateSchedule() {
    Preferences p;
    p.begin("dataupd", false);
    p.putBool("pending", true);
    p.end();
    setStatus(DU_RUNNING, "Khởi động lại để cập nhật...");
    delay(700); // let the HTTP reply / UI update flush
    ESP.restart();
}

// Read-AND-CLEAR the pending flag (clearing first avoids any boot-loop if the
// update later fails or crashes). Called once early in setup().
bool dataUpdatePending() {
    Preferences p;
    p.begin("dataupd", false);
    bool pend = p.getBool("pending", false);
    if (pend) p.putBool("pending", false);
    p.end();
    return pend;
}

// ---- OTA auto-check: compare the remote manifest version to the local one ----
static void extractVersion(const char *text, char *out, size_t cap) {
    if (cap) out[0] = '\0';
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, "version ", 8) == 0) {
            char v[24] = "";
            if (sscanf(p, "version %23s", v) == 1) {
                strncpy(out, v, cap - 1);
                out[cap - 1] = '\0';
            }
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

static void readLocalVersion(char *out, size_t cap) {
    if (cap) out[0] = '\0';
    char buf[96] = {0};
    int got = sdMgrReadFileChunk(DU_SPEEDMAP_DIR DU_MANIFEST_NAME, 0, (uint8_t *)buf, sizeof(buf) - 1);
    if (got > 0) {
        buf[got] = '\0';
        extractVersion(buf, out, cap);
    }
}

static void checkTask(void *) {
    String remoteText;
    String base = cfg.dataUpdateUrl;
    if (base.length() && !base.endsWith("/")) base += "/";
    readLocalVersion(s_localVersion, sizeof(s_localVersion));
    if (httpGetText(base + DU_MANIFEST_NAME, remoteText)) {
        extractVersion(remoteText.c_str(), s_remoteVersion, sizeof(s_remoteVersion));
        // "Update available" only for a NEWER release — a card carrying a more
        // recent local build must not be nagged into a downgrade.
        bool newer = s_remoteVersion[0] && strcmp(s_remoteVersion, s_localVersion) != 0 &&
                     !installerVersionOlder(s_remoteVersion, s_localVersion);
        s_updateAvailable = newer;
        Serial.printf("[dataupd] check: local=\"%s\" remote=\"%s\" -> %s\n",
                      s_localVersion, s_remoteVersion,
                      newer ? "UPDATE AVAILABLE" : "up to date");
    } else {
        Serial.println("[dataupd] check: could not fetch remote manifest");
    }
    s_checkRunning = false;
    vTaskDelete(NULL);
}

// Kick off a background version check. No-op if one is running, the URL is empty,
// WiFi station isn't connected, or an update is already flagged. Safe & cheap:
// only fetches the tiny manifest.txt.
bool dataUpdateCheckStart() {
    if (s_checkRunning || s_running) return false;
    if (strlen(cfg.dataUpdateUrl) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    s_checkRunning = true;
    xTaskCreatePinnedToCore(checkTask, "dataChk", 12288, NULL, 1, NULL, 0);
    return true;
}

bool dataUpdateAvailable() { return s_updateAvailable; }
bool dataUpdateCheckInProgress() { return s_checkRunning; }
const char *dataUpdateRemoteVersion() { return s_remoteVersion; }
const char *dataUpdateLocalVersion() { return s_localVersion; }

bool dataUpdateStart() {
    if (s_running) return false;
    if (strlen(cfg.dataUpdateUrl) == 0) {
        setStatus(DU_FAILED, "Chưa đặt địa chỉ dữ liệu");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        setStatus(DU_FAILED, "VietHUD chưa có Internet");
        return false;
    }
    s_running = true;
    // 12 KB stack: TLS + HTTPClient + ECDSA signature verify (the download buffer
    // is in PSRAM, not on the stack). Core 0, low priority — it spends its life blocked on the network.
    xTaskCreatePinnedToCore(dataUpdateTask, "dataUpd", 12288, NULL, 1, NULL, 0);
    return true;
}
