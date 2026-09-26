#pragma once
#include <stddef.h>
#include <stdint.h>
#include <Arduino.h> // String — JSON builders for the web API

// ---------------------------------------------------------------------------
// DataInstaller — the ONE path by which map/alert data on the SD card is ever
// replaced, whatever delivered the bytes:
//   * Phone Update Bridge (phone downloads from GitHub over 4G, uploads over
//     the device's own WiFi AP — net/UpdateApi.cpp), or
//   * Direct online update (device downloads itself — net/DataUpdater.cpp).
//
// Pipeline (docs/WIFI_PORTAL_UPDATE_BRIDGE_PLAN.md §8-§10, §16):
//
//   openSession(manifest.txt + ECDSA signature)   — signature MUST verify
//     -> needed files = remote entries whose sha differs from /speedmap/manifest.txt
//   write(name, offset, bytes...)  -> /vhupd/<name>.part   (resumable: offset == part size)
//   commit()  -> readback SHA-256 + size of every part -> rename .part -> .new
//             -> write /vhupd/commit.txt (THE commit point) -> caller reboots
//   [next boot, BEFORE sdMgrMount()]  bootApply(): journal redo
//             live -> /vhupd/<name>.bak, .new -> live   (idempotent, power-cut safe)
//   afterMount(ok): mount failed -> rollback (.bak -> live) + reboot
//   confirmHealthy() after ~60 s of normal running -> drop .bak files
//   3 boots without confirmation -> rollback (crash-loop guard)
//
// The live files are NEVER written in place and never swapped while the app is
// running (index/signs/cameras/names are cached in PSRAM at mount — swapping
// tiles.bin underneath them would desync). Not thread-safe by design: only the
// web task (bridge) or the update-mode task (direct) drives a session, never both.
// ---------------------------------------------------------------------------

enum InstallerState : uint8_t {
    INST_IDLE = 0,
    INST_RECEIVING,   // session open, parts incomplete
    INST_READY,       // every part has its full size
    INST_VERIFYING,
    INST_COMMITTED,   // journal written; reboot pending to apply
    INST_FAILED,
};

struct InstallerProgress {  // cheap, lock-free snapshot for the on-screen UI
    InstallerState state;
    uint32_t bytesDone;
    uint32_t bytesTotal;
    uint8_t filesDone;
    uint8_t filesTotal;
    char message[72];
};

// Result of the last install attempt, persisted on the card (/vhupd/last.txt)
// so both the web portal and the boot screen can report it after the reboot.
enum InstallerLastResult : uint8_t { INST_LAST_NONE = 0, INST_LAST_OK, INST_LAST_ROLLED_BACK };

// ---- session API (HTTP-ish status codes so the web layer can pass them on) ----
// 201 created/resumed, 200 nothing to update, 400 bad manifest, 401 bad
// signature, 409 busy (install pending confirmation), 507 no space, 503 no SD.
int installerOpenSession(const char *manifestText, size_t len, const char *sigB64, char *err, size_t errCap);
bool installerSessionActive();
const char *installerSessionId();
int installerFileCount();
bool installerFileInfo(int i, const char **name, uint32_t *size, uint32_t *received, uint8_t *datasetIdx);

// Chunk writes. begin() returns 200, 404 unknown file, 409 offset mismatch
// (*expected = where the client must resume), 410 no session.
int installerWriteBegin(const char *name, uint32_t offset, uint32_t *expected);
bool installerWriteData(const uint8_t *buf, size_t len);
uint32_t installerWriteEnd();   // closes the part, returns its new size

// Verify + stage + journal. 200 ok (reboot to apply), 412 parts incomplete,
// 422 checksum/size mismatch (bad part deleted, re-send it), 423 vehicle moving.
int installerCommit(bool checkMoving, char *err, size_t errCap);
void installerAbort();          // wipe the staging area

InstallerProgress installerProgress();

// ---- boot / lifecycle (main_viethud.cpp) ----
// Returns true if a pending commit was applied this boot.
bool installerBootApply();
void installerAfterMount(bool mountOk);   // may roll back + reboot (never returns then)
void installerConfirmHealthy();           // call once ~60 s after boot
InstallerLastResult installerLastResult(char *version, size_t cap, bool clear);

// ---- local state for UIs ----
// Local manifest version + per-dataset installed versions.
void installerLocalVersion(char *out, size_t cap);
int installerDatasetCount();
const char *installerDatasetId(int i);     // "map", "alerts"
const char *installerDatasetLabel(int i);  // Vietnamese label
void installerDatasetVersion(int i, char *out, size_t cap);

// JSON for GET /api/v1/update/state and /api/v1/update/session.
void installerStateJson(String &out);
void installerSessionJson(String &out);

// Signature check (also used by the direct updater before opening a session).
bool installerVerifySignature(const char *text, size_t len, const char *sigB64);

bool installerRolledBackThisBoot();   // a crash-loop rollback happened in bootApply()
bool installerTakeWifiOnRequest();    // read-and-clear: bring WiFi up after an install reboot
