#include "FwUpdater.h"
#include "core/Version.h"
#include "core/AppConfig.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_task_wdt.h>

static bool s_fwRunning = false;

bool fwUpdaterIsRunning() {
    return s_fwRunning;
}

// Simple JSON string value extractor
static bool extractJsonString(const char *json, const char *key, char *out, size_t maxLen) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '\"') {
        p++;
        const char *end = strchr(p, '\"');
        if (!end) return false;
        size_t len = end - p;
        if (len >= maxLen) len = maxLen - 1;
        strncpy(out, p, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

// Simple version comparator: returns >0 if v1 > v2, <0 if v1 < v2, 0 if equal
static int compareVersions(const char *v1, const char *v2) {
    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;
    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);
    if (maj1 != maj2) return maj1 - maj2;
    if (min1 != min2) return min1 - min2;
    return pat1 - pat2;
}

static bool fetchUrl(const char *url, String &payload, int timeoutMs = 4000) {
    if (WiFi.status() != WL_CONNECTED) return false;
    bool isHttps = (strncmp(url, "https://", 8) == 0);
    HTTPClient http;
    http.setTimeout(timeoutMs);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    bool ok = false;
    if (isHttps) {
        WiFiClientSecure sclient;
        sclient.setInsecure();
        if (http.begin(sclient, url)) {
            int code = http.GET();
            if (code == HTTP_CODE_OK) {
                payload = http.getString();
                ok = true;
            }
            http.end();
        }
    } else {
        WiFiClient client;
        if (http.begin(client, url)) {
            int code = http.GET();
            if (code == HTTP_CODE_OK) {
                payload = http.getString();
                ok = true;
            }
            http.end();
        }
    }
    return ok;
}

bool fwUpdaterCheck(FwUpdateInfo &info) {
    info.hasUpdate = false;
    strncpy(info.currentVersion, VIETHUD_FW_VERSION, sizeof(info.currentVersion) - 1);
    info.currentVersion[sizeof(info.currentVersion) - 1] = '\0';

    if (WiFi.status() != WL_CONNECTED) return false;

    // Step 1: Probe Raspberry Pi 4 on LAN first
    static const char *kPi4Endpoints[] = {
        "http://192.168.1.65/viethud/firmware/version.json",
        "http://homebridge.local/viethud/firmware/version.json"
    };

    String json;
    bool found = false;

    for (const char *ep : kPi4Endpoints) {
        Serial.printf("[fwupd] Probing Pi 4 at %s...\n", ep);
        if (fetchUrl(ep, json, 2500)) {
            char ver[16] = "";
            if (extractJsonString(json.c_str(), "version", ver, sizeof(ver))) {
                strncpy(info.remoteVersion, ver, sizeof(info.remoteVersion) - 1);
                char url[160] = "";
                if (extractJsonString(json.c_str(), "url_pi4", url, sizeof(url))) {
                    strncpy(info.downloadUrl, url, sizeof(info.downloadUrl) - 1);
                } else {
                    snprintf(info.downloadUrl, sizeof(info.downloadUrl), "http://192.168.1.65/viethud/firmware/%s/firmware.bin", ver);
                }
                extractJsonString(json.c_str(), "notes", info.releaseNotes, sizeof(info.releaseNotes));
                strncpy(info.sourceName, "Pi 4 (LAN)", sizeof(info.sourceName) - 1);
                found = true;
                Serial.printf("[fwupd] Found firmware %s on Pi 4!\n", ver);
                break;
            }
        }
    }

    // Step 2: Probe GitHub if Pi 4 not found or has no update
    if (!found) {
        static const char *kGithubUrl = "https://raw.githubusercontent.com/911273/VietHUD/main/firmware/version.json";
        Serial.printf("[fwupd] Probing GitHub at %s...\n", kGithubUrl);
        if (fetchUrl(kGithubUrl, json, 5000)) {
            char ver[16] = "";
            if (extractJsonString(json.c_str(), "version", ver, sizeof(ver))) {
                strncpy(info.remoteVersion, ver, sizeof(info.remoteVersion) - 1);
                char url[160] = "";
                if (extractJsonString(json.c_str(), "url_github", url, sizeof(url))) {
                    strncpy(info.downloadUrl, url, sizeof(info.downloadUrl) - 1);
                } else {
                    snprintf(info.downloadUrl, sizeof(info.downloadUrl), "https://raw.githubusercontent.com/911273/VietHUD/main/firmware/%s/firmware.bin", ver);
                }
                extractJsonString(json.c_str(), "notes", info.releaseNotes, sizeof(info.releaseNotes));
                strncpy(info.sourceName, "GitHub (Cloud)", sizeof(info.sourceName) - 1);
                found = true;
                Serial.printf("[fwupd] Found firmware %s on GitHub!\n", ver);
            }
        }
    }

    if (found) {
        if (compareVersions(info.remoteVersion, info.currentVersion) > 0) {
            info.hasUpdate = true;
        }
        return true;
    }

    return false;
}

bool fwUpdaterStart(const char *url, FwProgressCallback progressCb) {
    if (!url || !url[0] || WiFi.status() != WL_CONNECTED) return false;
    s_fwRunning = true;

    Serial.printf("[fwupd] Starting OTA download from: %s\n", url);
    if (progressCb) progressCb(0, "Connecting to firmware server...");

    bool isHttps = (strncmp(url, "https://", 8) == 0);
    HTTPClient http;
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    WiFiClientSecure sclient;
    WiFiClient client;
    bool beginOk = isHttps ? (sclient.setInsecure(), http.begin(sclient, url)) : http.begin(client, url);

    if (!beginOk) {
        s_fwRunning = false;
        if (progressCb) progressCb(-1, "HTTP connection failed");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[fwupd] HTTP GET failed, code %d\n", httpCode);
        http.end();
        s_fwRunning = false;
        if (progressCb) progressCb(-1, "Server returned error");
        return false;
    }

    int contentLength = http.getSize();
    Serial.printf("[fwupd] Content-Length: %d\n", contentLength);

    if (!Update.begin(contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN)) {
        Serial.println("[fwupd] Update.begin failed!");
        Update.printError(Serial);
        http.end();
        s_fwRunning = false;
        if (progressCb) progressCb(-1, "Not enough flash space");
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buff[2048];
    size_t written = 0;
    int lastPct = -1;

    if (progressCb) progressCb(0, "Flashing firmware...");

    while (http.connected() && (contentLength < 0 || written < (size_t)contentLength)) {
        size_t avail = stream->available();
        if (avail) {
            int readBytes = stream->readBytes(buff, sizeof(buff) < avail ? sizeof(buff) : avail);
            if (readBytes > 0) {
                if (Update.write(buff, readBytes) != (size_t)readBytes) {
                    Update.printError(Serial);
                    Update.abort();
                    http.end();
                    s_fwRunning = false;
                    if (progressCb) progressCb(-1, "Flash write error");
                    return false;
                }
                written += readBytes;
                if (contentLength > 0) {
                    int pct = (int)((uint64_t)written * 100 / contentLength);
                    if (pct != lastPct) {
                        lastPct = pct;
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Flashing: %d%% (%u KB)", pct, (unsigned)(written / 1024));
                        if (progressCb) progressCb(pct, msg);
                    }
                }
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(1);
    }

    http.end();

    if (!Update.end(true)) {
        Serial.println("[fwupd] Update.end failed!");
        Update.printError(Serial);
        s_fwRunning = false;
        if (progressCb) progressCb(-1, "Verification failed");
        return false;
    }

    Serial.printf("[fwupd] OTA successful! Written %u bytes. Rebooting...\n", (unsigned)written);
    if (progressCb) progressCb(100, "Update successful! Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
    return true;
}
