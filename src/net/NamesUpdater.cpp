#include "NamesUpdater.h"
#include "boards/board.h"
#include "map/RoadNames.h"
#include "map/SdCardManager.h"
#include "update/DataInstaller.h" // installerVerifySignature(), installerVersionOlder()
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <string.h>

#define NAMES_BASE "https://raw.githubusercontent.com/911273/VietHUD/main/" BOARD_NAMES_CHANNEL "/"

static bool sAvailable = false;
static char sRemoteVersion[24] = "";

static void localPath(char *out, size_t cap, const char *file) { snprintf(out, cap, "%s/%s", sdMgrGetBaseDir(), file); }

static bool getText(const String &url, String &out) {
    WiFiClientSecure sclient;
    sclient.setInsecure(); // the manifest is signed and the file SHA-256 checked
    HTTPClient http;
    if (!http.begin(sclient, url)) return false;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    int code = http.GET();
    bool ok = code == 200;
    if (ok) out = http.getString();
    else Serial.printf("[names] GET %s -> %d\n", url.c_str(), code);
    http.end();
    return ok;
}

// Signed manifest -> version, size, sha. False if unsigned / malformed.
static bool fetchManifest(char *ver, size_t verCap, uint32_t *size, char sha[65]) {
    String m, sig;
    if (!getText(NAMES_BASE "manifest.txt", m) || !getText(NAMES_BASE "manifest.txt.sig", sig)) return false;
    sig.trim();
    if (!installerVerifySignature(m.c_str(), m.length(), sig.c_str())) {
        Serial.println("[names] manifest signature INVALID — ignored");
        return false;
    }
    char v[24] = "", name[32] = "";
    unsigned long sz = 0;
    if (sscanf(m.c_str(), "version %23s\n%31s %lu %64s", v, name, &sz, sha) != 4 || strcmp(name, "roadnames.bin") != 0)
        return false;
    strncpy(ver, v, verCap - 1);
    ver[verCap - 1] = '\0';
    *size = (uint32_t)sz;
    return true;
}

void namesLocalVersion(char *out, size_t cap) {
    out[0] = '\0';
    char p[80];
    localPath(p, sizeof(p), "roadnames.ver");
    int n = sdMgrReadFileChunk(p, 0, (uint8_t *)out, cap - 1);
    if (n <= 0) return out[0] = '\0', void();
    out[n] = '\0';
    for (int i = 0; out[i]; i++)
        if (out[i] == '\n' || out[i] == '\r' || out[i] == ' ') { out[i] = '\0'; break; }
}

static bool writable() {
#if BOARD_FLASH_DATA_READONLY
    if (sdMgrDataOnFlash()) return false; // P4 without a card
#endif
    return sdMgrIsAvailable();
}

bool namesCheck() {
    sAvailable = false;
    if (!writable() || WiFi.status() != WL_CONNECTED) return false;
    uint32_t size;
    char sha[65], local[24];
    if (!fetchManifest(sRemoteVersion, sizeof(sRemoteVersion), &size, sha)) return false;
    namesLocalVersion(local, sizeof(local));
    sAvailable = strcmp(sRemoteVersion, local) != 0 && !installerVersionOlder(sRemoteVersion, local);
    Serial.printf("[names] check: local=\"%s\" remote=\"%s\" -> %s\n", local, sRemoteVersion,
                  sAvailable ? "UPDATE AVAILABLE" : "up to date");
    return sAvailable;
}

bool namesUpdateAvailable() { return sAvailable; }
const char *namesRemoteVersion() { return sRemoteVersion; }

int namesUpdateRun(uint8_t *buf, size_t bufSize, void (*progress)(int, const char *)) {
    if (!writable()) return 0;
    char ver[24], sha[65], local[24];
    uint32_t size = 0;
    if (!fetchManifest(ver, sizeof(ver), &size, sha)) return 0;
    namesLocalVersion(local, sizeof(local));
    if (strcmp(ver, local) == 0 || installerVersionOlder(ver, local)) return 2;

    char part[80], dst[80], verPath[80];
    localPath(part, sizeof(part), "roadnames.bin.part");
    localPath(dst, sizeof(dst), "roadnames.bin");
    localPath(verPath, sizeof(verPath), "roadnames.ver");
    int64_t have = sdMgrFileSize(part);
    if (have < 0 || have > size) {
        sdMgrRemove(part);
        have = 0;
    }
    if (have < size) {
        uint64_t freeB = sdMgrFreeBytes();
        if (freeB < (uint64_t)(size - have) + 256 * 1024) {
            Serial.printf("[names] not enough space: need %u KB, %u KB free\n", (unsigned)((size - have) / 1024),
                          (unsigned)(freeB / 1024));
            if (progress) progress(-1, "Not enough space for road names");
            return 0;
        }
        for (int attempt = 0; attempt < 3 && have < size; attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(2000));
            WiFiClientSecure sclient;
            sclient.setInsecure();
            HTTPClient http;
            if (!http.begin(sclient, NAMES_BASE "roadnames.bin")) continue;
            http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            http.setTimeout(15000);
            if (have > 0) http.addHeader("Range", String("bytes=") + (uint32_t)have + "-");
            int code = http.GET();
            if (code != 200 && code != 206) {
                Serial.printf("[names] download HTTP %d\n", code);
                http.end();
                continue;
            }
            if (code == 200 && have > 0) { // server ignored Range: start over
                sdMgrRemove(part);
                have = 0;
            }
            if (!sdMgrWriterOpen(part, true)) {
                http.end();
                return 0;
            }
            WiFiClient *s = http.getStreamPtr();
            uint32_t lastData = millis();
            while (have < size && http.connected() && millis() - lastData < 15000) {
                size_t avail = s->available();
                if (!avail) {
                    vTaskDelay(1);
                    continue;
                }
                int r = s->readBytes(buf, avail < bufSize ? avail : bufSize);
                if (r <= 0) continue;
                if (!sdMgrWriterWrite(buf, r)) break;
                have += r;
                lastData = millis();
                esp_task_wdt_reset();
                if (progress) {
                    char m[48];
                    snprintf(m, sizeof(m), "Road names %u%%", (unsigned)(have * 100 / size));
                    progress((int)(have * 100 / size), m);
                }
            }
            sdMgrWriterClose();
            http.end();
        }
        if (have < size) {
            if (progress) progress(-1, "Road names download failed");
            return 0; // the part stays: the next run resumes it
        }
    }
    // Verify, then swap in.
    if (progress) progress(100, "Checking road names...");
    uint8_t h[32];
    char hex[65];
    if (!sdMgrSha256File(part, h, [] { esp_task_wdt_reset(); })) return 0;
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
    if (strcasecmp(hex, sha) != 0) {
        Serial.println("[names] SHA-256 mismatch — discarded");
        sdMgrRemove(part);
        if (progress) progress(-1, "Road names corrupt — discarded");
        return 0;
    }
    roadNamesReset();
    if (!sdMgrRename(part, dst)) return 0;
    char vline[32];
    int vl = snprintf(vline, sizeof(vline), "%s\n", ver);
    sdMgrWriteSmallFile(verPath, vline, vl);
    Serial.printf("[names] installed road names %s (%u bytes)\n", ver, (unsigned)size);
    sAvailable = false;
    return 1;
}
