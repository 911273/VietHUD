#pragma once
#include <Arduino.h>

struct FwUpdateInfo {
    bool hasUpdate = false;
    char remoteVersion[16] = "";
    char currentVersion[16] = "";
    char downloadUrl[160] = "";
    char sourceName[32] = "";
    char releaseNotes[128] = "";
};

typedef void (*FwProgressCallback)(int percent, const char *status);

// Probes Pi 4 LAN and GitHub to check for newer firmware version
bool fwUpdaterCheck(FwUpdateInfo &info);

// Performs OTA firmware update from the given binary URL
bool fwUpdaterStart(const char *url, FwProgressCallback progressCb);

// Check if firmware OTA is currently in progress
bool fwUpdaterIsRunning();
