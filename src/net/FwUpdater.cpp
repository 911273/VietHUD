#include "FwUpdater.h"
#include "core/Version.h"
#include "core/AppConfig.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <esp_app_format.h> // esp_image_header_t — chip id of a downloaded image
#include <mbedtls/sha256.h>

// One release for every VietHUD model (docs/VIETHUD_PLATFORM.md "Firmware"):
// tools/release.py publishes firmware/release.json —
//   {"version": "4.0.0", "notes": "...",
//    "images": {"viethud35": {"chip": "esp32s3", "path": "4.0.0/viethud35.bin",
//                             "size": 2240457, "sha256": "..."}, ...}}
// The device looks itself up by VIETHUD_MODEL_ID, so the user publishes ONE
// release and each unit picks the image built for its own hardware. The
// download is then checked against the hardware three ways before it can boot:
//   1. the image header's chip id must be this chip (S3 vs P4),
//   2. the image must carry this model's identity marker (kModelMarker below,
//      compiled into every build) — catches a wrong file behind a right name,
//   3. its SHA-256 must match release.json (truncated/corrupt download).
// Any mismatch aborts the OTA; the running firmware stays.
//
// Units on firmware before 4.0 only know their channel's version.json
// (firmware/<model>/version.json); tools/release.py keeps writing those too,
// and this firmware falls back to them when release.json isn't there.

// Identity marker searched for in a downloaded image (referenced in
// fwUpdaterCheck() so the linker keeps it).
#define VH_MARKER_PREFIX "VIETHUD-HW-ID:"
static const char kModelMarker[] = VH_MARKER_PREFIX VIETHUD_MODEL_ID ";";
// Version marker, read by tools/update_tool over USB to tell what a unit runs.
static const char kFwMarker[] = "VIETHUD-FW:" VIETHUD_FW_VERSION ";";

static bool s_fwRunning = false;

bool fwUpdaterIsRunning() {
    return s_fwRunning;
}

// Simple JSON value extractor (string or bare number) — release.json is ours
// and flat enough for this; `json` may be a sub-range (see findModelObject).
static bool extractJsonValue(const char *json, const char *key, char *out, size_t maxLen, const char *end = nullptr) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p || (end && p >= end)) return false;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    const char *stop;
    if (*p == '\"') {
        p++;
        stop = strchr(p, '\"');
        if (!stop) return false;
    } else {
        stop = p;
        while (*stop && *stop != ',' && *stop != '}' && *stop != '\r' && *stop != '\n') stop++;
    }
    size_t len = stop - p;
    if (len >= maxLen) len = maxLen - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return true;
}
static bool extractJsonString(const char *json, const char *key, char *out, size_t maxLen) {
    return extractJsonValue(json, key, out, maxLen);
}

// The {...} object for "<model>" inside "images", as [begin, end).
static bool findModelObject(const char *json, const char *model, const char **begin, const char **end) {
    const char *images = strstr(json, "\"images\"");
    if (!images) return false;
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", model);
    const char *p = strstr(images, pattern);
    if (!p) return false;
    p = strchr(p, '{');
    if (!p) return false;
    const char *e = strchr(p, '}');
    if (!e) return false;
    *begin = p;
    *end = e;
    return true;
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

// Download roots, tried in order: the Pi (LAN, mDNS, Tailscale — a phone
// hotspot away from home still reaches it), then GitHub. Each serves the same
// tree: <root>firmware/release.json, <root>firmware/<version>/<model>.bin.
static const struct {
    const char *root;
    const char *legacyUrlKey; // per-channel version.json key with this route's URL
    const char *name;
    int timeoutMs;
} kSources[] = {
    {"http://192.168.1.65/viethud/", "url_pi4", "Pi 4 (LAN)", 2500},
    {"http://homebridge.local/viethud/", "url_pi4_mdns", "Pi 4 (mDNS)", 2500},
    {"http://100.107.34.92/viethud/", "url_pi4_tailscale", "Pi 4 (Tailscale)", 2500},
    {"https://raw.githubusercontent.com/911273/VietHUD/main/", "url_github", "GitHub (Cloud)", 5000},
};

// release.json -> this model's entry. False when the file has no image for
// this hardware (then nothing is offered).
static bool parseRelease(const String &json, const char *root, FwUpdateInfo &info) {
    char ver[16] = "";
    if (!extractJsonString(json.c_str(), "version", ver, sizeof(ver))) return false;
    const char *b, *e;
    if (!findModelObject(json.c_str(), VIETHUD_MODEL_ID, &b, &e)) {
        Serial.printf("[fwupd] release %s has no image for model \"%s\"\n", ver, VIETHUD_MODEL_ID);
        return false;
    }
    char path[96] = "", sha[65] = "", size[16] = "", chip[16] = "";
    if (!extractJsonValue(b, "path", path, sizeof(path), e)) return false;
    extractJsonValue(b, "sha256", sha, sizeof(sha), e);
    extractJsonValue(b, "size", size, sizeof(size), e);
    extractJsonValue(b, "chip", chip, sizeof(chip), e);
    if (chip[0] && strcmp(chip, CONFIG_IDF_TARGET) != 0) {
        Serial.printf("[fwupd] REFUSED: image for %s is built for chip %s, this is %s\n", VIETHUD_MODEL_ID, chip,
                      CONFIG_IDF_TARGET);
        return false;
    }
    strncpy(info.remoteVersion, ver, sizeof(info.remoteVersion) - 1);
    snprintf(info.downloadUrl, sizeof(info.downloadUrl), "%sfirmware/%s", root, path);
    strncpy(info.sha256, sha, sizeof(info.sha256) - 1);
    info.size = (uint32_t)strtoul(size, nullptr, 10);
    extractJsonString(json.c_str(), "notes", info.releaseNotes, sizeof(info.releaseNotes));
    return true;
}

// Pre-4.0 per-channel version.json (firmware/<model>/version.json).
static bool parseLegacy(const String &json, const char *root, const char *urlKey, FwUpdateInfo &info) {
    char ver[16] = "", model[24] = "";
    if (!extractJsonString(json.c_str(), "version", ver, sizeof(ver))) return false;
    // Wrong-product guard: an image for another VietHUD model (or the old
    // stand-alone Lite, whose version.json has no "model") is never offered.
    extractJsonString(json.c_str(), "model", model, sizeof(model));
    if (strcmp(model, VIETHUD_MODEL_ID) != 0) {
        Serial.printf("[fwupd] REFUSED: version.json is for model \"%s\", this device is \"%s\"\n",
                      model[0] ? model : "(none)", VIETHUD_MODEL_ID);
        return false;
    }
    strncpy(info.remoteVersion, ver, sizeof(info.remoteVersion) - 1);
    if (!extractJsonString(json.c_str(), urlKey, info.downloadUrl, sizeof(info.downloadUrl)))
        snprintf(info.downloadUrl, sizeof(info.downloadUrl), "%s" VIETHUD_FW_CHANNEL "/%s/firmware.bin", root, ver);
    extractJsonString(json.c_str(), "notes", info.releaseNotes, sizeof(info.releaseNotes));
    info.sha256[0] = '\0';
    info.size = 0;
    return true;
}

bool fwUpdaterCheck(FwUpdateInfo &info) {
    info = FwUpdateInfo();
    strncpy(info.currentVersion, VIETHUD_FW_VERSION, sizeof(info.currentVersion) - 1);
    // Also a real reference to the marker: without one the linker's
    // --gc-sections drops it from the image, and the next OTA's check 2 fails.
    Serial.printf("[fwupd] %s %s\n", kModelMarker, kFwMarker);
    if (WiFi.status() != WL_CONNECTED) return false;

    String json;
    bool found = false;
    for (const auto &src : kSources) {
        char url[160];
        snprintf(url, sizeof(url), "%sfirmware/release.json", src.root);
        Serial.printf("[fwupd] %s: %s\n", src.name, url);
        if (fetchUrl(url, json, src.timeoutMs) && parseRelease(json, src.root, info)) {
            found = true;
        } else {
            snprintf(url, sizeof(url), "%s" VIETHUD_FW_CHANNEL "/version.json", src.root);
            if (fetchUrl(url, json, src.timeoutMs) && parseLegacy(json, src.root, src.legacyUrlKey, info)) found = true;
        }
        if (found) {
            strncpy(info.sourceName, src.name, sizeof(info.sourceName) - 1);
            Serial.printf("[fwupd] %s offers %s for %s (%s)\n", src.name, info.remoteVersion, VIETHUD_MODEL_ID,
                          info.downloadUrl);
            break;
        }
    }
    if (!found) return false;
    if (compareVersions(info.remoteVersion, info.currentVersion) > 0) info.hasUpdate = true;
    return true;
}

// Streaming search for VH_MARKER_PREFIX "<model>;" across chunk boundaries.
struct MarkerScan {
    char tail[64] = "";
    size_t tailLen = 0;
    char found[32] = ""; // model id of the first marker seen
    void feed(const uint8_t *data, size_t len) {
        if (found[0]) return;
        // tail (the last bytes of the stream so far, however many chunks they
        // came in) + the start of this chunk, so a marker cut anywhere is seen
        uint8_t joint[sizeof(tail) * 2];
        size_t head = len < sizeof(tail) ? len : sizeof(tail);
        memcpy(joint, tail, tailLen);
        memcpy(joint + tailLen, data, head);
        if (search(joint, tailLen + head)) return;
        if (search(data, len)) return;
        // new tail = last sizeof(tail) bytes of (old tail + data)
        if (len >= sizeof(tail)) {
            memcpy(tail, data + len - sizeof(tail), sizeof(tail));
            tailLen = sizeof(tail);
        } else {
            size_t total = tailLen + len, drop = total > sizeof(tail) ? total - sizeof(tail) : 0;
            memmove(tail, tail + drop, tailLen - drop);
            memcpy(tail + tailLen - drop, data, len);
            tailLen = total - drop;
        }
    }
    bool search(const uint8_t *d, size_t n) {
        static const size_t kP = sizeof(VH_MARKER_PREFIX) - 1;
        for (size_t i = 0; i + kP < n; i++) {
            if (d[i] != 'V' || memcmp(d + i, VH_MARKER_PREFIX, kP) != 0) continue;
            // Model ids are [a-z0-9]+ — anything else is the bare prefix
            // literal this very code uses, not a marker.
            size_t j = i + kP, k = 0;
            while (j < n && k < sizeof(found) - 1 && ((d[j] >= 'a' && d[j] <= 'z') || (d[j] >= '0' && d[j] <= '9')))
                found[k++] = (char)d[j++];
            if (k > 0 && j < n && d[j] == ';') {
                found[k] = '\0';
                return true;
            }
            found[0] = '\0'; // cut by the chunk end — the joint pass sees it whole
        }
        return false;
    }
};

bool fwUpdaterStart(const char *url, FwProgressCallback progressCb, const char *expectSha256) {
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

    auto fail = [&](const char *msg) {
        Update.abort();
        http.end();
        s_fwRunning = false;
        Serial.printf("[fwupd] FAILED: %s\n", msg);
        if (progressCb) progressCb(-1, msg);
        return false;
    };

    if (!beginOk) {
        s_fwRunning = false;
        if (progressCb) progressCb(-1, "HTTP connection failed");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[fwupd] HTTP GET failed, code %d\n", httpCode);
        return fail("Server returned error");
    }

    int contentLength = http.getSize();
    Serial.printf("[fwupd] Content-Length: %d\n", contentLength);

    if (!Update.begin(contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        return fail("Not enough flash space");
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    MarkerScan marker;
    bool headerChecked = false;

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
                // Hardware check 1: chip id in the image header (first chunk).
                if (!headerChecked && written == 0 && (size_t)readBytes >= sizeof(esp_image_header_t)) {
                    const esp_image_header_t *h = (const esp_image_header_t *)buff;
                    if (h->magic != ESP_IMAGE_HEADER_MAGIC || h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                        Serial.printf("[fwupd] image chip id %u, this chip %u\n", (unsigned)h->chip_id,
                                      (unsigned)CONFIG_IDF_FIRMWARE_CHIP_ID);
                        mbedtls_sha256_free(&sha);
                        return fail("Firmware is for another chip");
                    }
                    headerChecked = true;
                }
                mbedtls_sha256_update(&sha, buff, readBytes);
                marker.feed(buff, readBytes);
                if (Update.write(buff, readBytes) != (size_t)readBytes) {
                    Update.printError(Serial);
                    mbedtls_sha256_free(&sha);
                    return fail("Flash write error");
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

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    // Hardware check 2: the image is built for THIS model.
    if (strcmp(marker.found, VIETHUD_MODEL_ID) != 0) {
        Serial.printf("[fwupd] image model \"%s\", this device \"%s\"\n", marker.found[0] ? marker.found : "(none)",
                      VIETHUD_MODEL_ID);
        return fail("Firmware is for another model");
    }
    // Check 3: integrity against release.json.
    if (expectSha256 && expectSha256[0]) {
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (strcasecmp(hex, expectSha256) != 0) return fail("Checksum mismatch");
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
