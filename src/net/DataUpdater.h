#pragma once
#include <stdint.h>
#include <stddef.h>

// Online updater for the map + traffic-warning data (2026-09-25). Fetches a
// plain-text manifest from cfg.dataUpdateUrl (the user's Raspberry Pi, which
// bridges their Google Drive), compares each file's SHA-256 to the copy already
// on the SD card, and downloads only what changed — streamed straight to the
// card, verified, then atomically renamed over the live file. See the approved
// plan and net/DataUpdater.cpp for the full flow.
//
// Data is vector-only: tiles/index/metadata/names/seg_names/signs/cameras.

enum DataUpdateState : uint8_t {
    DU_IDLE = 0,   // never run this session, or finished a while ago
    DU_RUNNING,    // download in progress
    DU_SUCCESS,    // finished OK (device reboots shortly after to reload)
    DU_FAILED      // stopped on an error — old data left intact
};

struct DataUpdateStatus {
    DataUpdateState state;
    char message[96];  // human-readable current step or error reason (UTF-8 Vietnamese)
    int filesTotal;    // files that need updating this run
    int filesDone;     // files completed so far
    int percent;       // 0..100 progress of the CURRENT file
};

// Starts an update in the background (its own one-shot FreeRTOS task). No-op if
// one is already running, cfg.dataUpdateUrl is empty, or WiFi station isn't
// connected. Returns false (and sets a FAILED status message) in those cases.
// Used by "update mode" at boot, where there's enough free RAM for TLS.
bool dataUpdateStart();

// Schedule an update: set an NVS flag + reboot into "update mode" (the reliable
// path — TLS needs contiguous RAM the running app doesn't leave free). The web
// and on-screen "Update data" buttons call this.
void dataUpdateSchedule();
// Read-and-clear the pending-update flag; called once at boot to decide whether
// to enter update mode.
bool dataUpdatePending();

// Snapshot of the current progress/result for the web + on-screen UI.
DataUpdateStatus dataUpdateGetStatus();

// OTA auto-check (2026-09-26): fetch ONLY the remote manifest.txt and compare its
// version line to the copy on the SD card. Cheap, downloads nothing else, never
// reboots. dataUpdateCheckStart() runs it in the background (no-op unless WiFi
// station is connected and a URL is set); the getters feed the on-screen + web
// "update available" indicator. Applying an update is still the manual
// dataUpdateSchedule() path.
bool dataUpdateCheckStart();
bool dataUpdateAvailable();
bool dataUpdateCheckInProgress();
const char *dataUpdateRemoteVersion();
const char *dataUpdateLocalVersion();
