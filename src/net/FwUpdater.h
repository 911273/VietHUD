#pragma once
#include <Arduino.h>

struct FwUpdateInfo {
    bool hasUpdate = false;
    char remoteVersion[16] = "";
    char currentVersion[16] = "";
    char downloadUrl[160] = "";
    char sourceName[32] = "";
    char releaseNotes[128] = "";
    char sha256[65] = ""; // from release.json; empty for a pre-4.0 channel version.json
    uint32_t size = 0;
};

typedef void (*FwProgressCallback)(int percent, const char *status);

// Probes the Pi 4 (LAN/mDNS/Tailscale) then GitHub for firmware/release.json —
// one release for every model — and picks the image for THIS hardware
// (VIETHUD_MODEL_ID). Falls back to the pre-4.0 per-model version.json.
bool fwUpdaterCheck(FwUpdateInfo &info);

// Performs the OTA from the given image URL. Refuses (and keeps the running
// firmware) unless the image is for this chip AND this model, and — when
// expectSha256 is given — matches it.
bool fwUpdaterStart(const char *url, FwProgressCallback progressCb, const char *expectSha256 = nullptr);

// Check if firmware OTA is currently in progress
bool fwUpdaterIsRunning();
