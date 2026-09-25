#include "DataUpdater.h"
#include "core/AppConfig.h"
#include "map/SdCardManager.h"
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
static volatile bool s_running = false;

DataUpdateStatus dataUpdateGetStatus() { return s_status; }

static void setStatus(DataUpdateState st, const char *msg) {
    s_status.state = st;
    strncpy(s_status.message, msg ? msg : "", sizeof(s_status.message) - 1);
    s_status.message[sizeof(s_status.message) - 1] = '\0';
    Serial.printf("[dataupd] %s\n", s_status.message);
}

struct MFile {
    char name[40];
    char sha[65];
};

// Parse a manifest text blob: a "version <x>" line plus "<name> <size> <sha>"
// lines. Fills out[] (name+sha only; size is informational) and returns count.
static int parseManifest(const char *text, MFile *out, int maxN, char *verOut, size_t verCap) {
    int n = 0;
    if (verOut && verCap) verOut[0] = '\0';
    const char *p = text;
    while (*p && n < maxN) {
        // one line into a temp
        char line[160];
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;
        if (line[0] == '\0' || line[0] == '#') continue;
        char a[48], b[24], c[80];
        if (sscanf(line, "%47s %23s %79s", a, b, c) == 3) {
            if (strcmp(a, "version") == 0) continue; // "version <x> <y>" unlikely, but guard
            strncpy(out[n].name, a, sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = '\0';
            strncpy(out[n].sha, c, sizeof(out[n].sha) - 1);
            out[n].sha[sizeof(out[n].sha) - 1] = '\0';
            n++;
        } else if (sscanf(line, "version %79s", c) == 1 && verOut) {
            strncpy(verOut, c, verCap - 1);
            verOut[verCap - 1] = '\0';
        }
    }
    return n;
}

static const char *shaFor(const MFile *arr, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(arr[i].name, name) == 0) return arr[i].sha;
    return "";
}

static void hexEncode(const uint8_t *in, int len, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xF];
    }
    out[len * 2] = '\0';
}

// HTTP(S) GET into a String (small responses only — the manifest). Returns true
// on 200. Uses an insecure TLS client for https (no cert store on-device).
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

// Downloads one file, streaming to /speedmap/<name>.tmp while hashing, verifies
// SHA-256, then atomically renames over the live file. Returns true on success.
static bool downloadFile(const String &base, const char *name, const char *expectSha, uint8_t *buf,
                         size_t bufSize) {
    String url = base + name;
    char curPath[64], newPath[72];
    snprintf(curPath, sizeof(curPath), DU_SPEEDMAP_DIR "%s", name);
    snprintf(newPath, sizeof(newPath), DU_SPEEDMAP_DIR "%s.tmp", name);
    sdMgrRemove(newPath); // clear any stale partial

    WiFiClientSecure sclient;
    WiFiClient client;
    bool https = url.startsWith("https");
    if (https) sclient.setInsecure();
    HTTPClient http;
    if (!http.begin(https ? (WiFiClient &)sclient : client, url)) {
        setStatus(DU_FAILED, "begin() failed");
        return false;
    }
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // GitHub raw 301/302s to the Fastly CDN
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        char m[64];
        snprintf(m, sizeof(m), "HTTP %d for %s", code, name);
        setStatus(DU_FAILED, m);
        http.end();
        return false;
    }
    int contentLen = http.getSize(); // may be -1 (chunked)
    WiFiClient *stream = http.getStreamPtr();

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256 (not 224)

    int total = 0;
    uint32_t lastDataMs = millis();
    bool ok = true;
    while (http.connected() && (contentLen < 0 || total < contentLen)) {
        size_t avail = stream->available();
        if (avail) {
            int r = stream->readBytes(buf, avail > bufSize ? bufSize : avail);
            if (r > 0) {
                mbedtls_sha256_update(&ctx, buf, r);
                if (!sdMgrAppendBytes(newPath, buf, r)) {
                    setStatus(DU_FAILED, "SD write failed");
                    ok = false;
                    break;
                }
                total += r;
                lastDataMs = millis();
                if (contentLen > 0) s_status.percent = (int)((int64_t)total * 100 / contentLen);
            }
        } else {
            if (millis() - lastDataMs > 15000) {
                setStatus(DU_FAILED, "download stalled");
                ok = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    http.end();

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    if (!ok) {
        sdMgrRemove(newPath);
        return false;
    }
    char got[65];
    hexEncode(hash, 32, got);
    if (strcasecmp(got, expectSha) != 0) {
        setStatus(DU_FAILED, "checksum mismatch");
        Serial.printf("[dataupd] %s sha got=%s want=%s\n", name, got, expectSha);
        sdMgrRemove(newPath);
        return false;
    }
    // Verified — atomically replace the live file.
    if (!sdMgrRename(newPath, curPath)) {
        setStatus(DU_FAILED, "rename failed");
        sdMgrRemove(newPath);
        return false;
    }
    Serial.printf("[dataupd] updated %s (%d bytes)\n", name, total);
    return true;
}

// Returns: 0 = failed, 1 = success WITH changes (caller reboots), 2 = success no
// change. Work buffers are passed in (PSRAM, allocated by the task) so nothing
// large sits in scarce internal RAM while WiFi/TLS is up.
static int runUpdate(uint8_t *dlBuf, size_t dlBufSize, MFile *remote, MFile *local, uint8_t *lbuf,
                     size_t lbufSize) {
    s_status.filesTotal = 0;
    s_status.filesDone = 0;
    s_status.percent = 0;
    setStatus(DU_RUNNING, "Fetching manifest...");

    String base = cfg.dataUpdateUrl;
    if (base.length() && !base.endsWith("/")) base += "/";

    // Connectivity diagnostics: STA IP + a DNS test of the host, so a failure is
    // pinpointed (DNS poisoned by our own captive server -> resolves to
    // 192.168.4.1 = wrong; DNS ok but fetch fails = TLS/memory).
    Serial.printf("[dataupd] STA ip=%s dns=%s  url=%s\n", WiFi.localIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(), (base + DU_MANIFEST_NAME).c_str());
    {
        // Extract host from the base URL for a DNS probe.
        String host = base;
        int p = host.indexOf("://");
        if (p >= 0) host = host.substring(p + 3);
        int slash = host.indexOf('/');
        if (slash >= 0) host = host.substring(0, slash);
        IPAddress ip;
        if (WiFi.hostByName(host.c_str(), ip))
            Serial.printf("[dataupd] DNS %s -> %s\n", host.c_str(), ip.toString().c_str());
        else
            Serial.printf("[dataupd] DNS FAILED for %s\n", host.c_str());
    }

    String remoteText;
    if (!httpGetText(base + DU_MANIFEST_NAME, remoteText)) {
        setStatus(DU_FAILED, "Cannot fetch manifest");
        return 0;
    }
    char remoteVer[32];
    int rn = parseManifest(remoteText.c_str(), remote, DU_MAX_FILES, remoteVer, sizeof(remoteVer));
    if (rn == 0) {
        setStatus(DU_FAILED, "Empty/invalid manifest");
        return 0;
    }

    // Local manifest (may be absent -> everything is "new").
    int ln = 0;
    int got = sdMgrReadFileChunk(DU_SPEEDMAP_DIR DU_MANIFEST_NAME, 0, lbuf, lbufSize - 1);
    if (got > 0) {
        lbuf[got] = '\0';
        ln = parseManifest((const char *)lbuf, local, DU_MAX_FILES, nullptr, 0);
    }

    // Which files differ?
    int need[DU_MAX_FILES], needN = 0;
    for (int i = 0; i < rn; i++)
        if (strcasecmp(remote[i].sha, shaFor(local, ln, remote[i].name)) != 0) need[needN++] = i;
    s_status.filesTotal = needN;
    if (needN == 0) {
        setStatus(DU_SUCCESS, "Already up to date");
        return 2;
    }

    for (int k = 0; k < needN; k++) {
        int idx = need[k];
        s_status.filesDone = k;
        s_status.percent = 0;
        char m[64];
        snprintf(m, sizeof(m), "Downloading %s (%d/%d)", remote[idx].name, k + 1, needN);
        setStatus(DU_RUNNING, m);
        if (!downloadFile(base, remote[idx].name, remote[idx].sha, dlBuf, dlBufSize)) return 0; // FAILED set inside
        s_status.filesDone = k + 1;
    }

    // Persist the new manifest so the next run diffs against it.
    sdMgrRemove(DU_SPEEDMAP_DIR DU_MANIFEST_NAME);
    sdMgrAppendBytes(DU_SPEEDMAP_DIR DU_MANIFEST_NAME, (const uint8_t *)remoteText.c_str(), remoteText.length());

    char done[64];
    snprintf(done, sizeof(done), "Updated %d file(s) - rebooting", needN);
    setStatus(DU_SUCCESS, done);
    return 1;
}

static void dataUpdateTask(void *) {
    // All big scratch in PSRAM so internal RAM stays free for WiFi/TLS.
    const size_t DL = 4096;
    uint8_t *dlBuf = (uint8_t *)heap_caps_malloc(DL, MALLOC_CAP_SPIRAM);
    uint8_t *lbuf = (uint8_t *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    MFile *remote = (MFile *)heap_caps_malloc(sizeof(MFile) * DU_MAX_FILES, MALLOC_CAP_SPIRAM);
    MFile *local = (MFile *)heap_caps_malloc(sizeof(MFile) * DU_MAX_FILES, MALLOC_CAP_SPIRAM);

    int result = 0;
    if (dlBuf && lbuf && remote && local) result = runUpdate(dlBuf, DL, remote, local, lbuf, 2048);
    else setStatus(DU_FAILED, "Out of memory");

    if (dlBuf) heap_caps_free(dlBuf);
    if (lbuf) heap_caps_free(lbuf);
    if (remote) heap_caps_free(remote);
    if (local) heap_caps_free(local);

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
    setStatus(DU_RUNNING, "Khoi dong lai de cap nhat...");
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

bool dataUpdateStart() {
    if (s_running) return false;
    if (strlen(cfg.dataUpdateUrl) == 0) {
        setStatus(DU_FAILED, "No update URL set (Config)");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        setStatus(DU_FAILED, "No internet (WiFi station)");
        return false;
    }
    s_running = true;
    // 8 KB stack: TLS + HTTPClient + the 4 KB download buffer is static, not on
    // the stack. Core 0, low priority — it spends its life blocked on the network.
    xTaskCreatePinnedToCore(dataUpdateTask, "dataUpd", 8192, NULL, 1, NULL, 0);
    return true;
}
