#pragma once
#include "SpeedMapFormat.h"
#include <stddef.h> // size_t — sdMgrReadFileChunk()'s offset/length parameters

// Sole owner of the microSD card (spec section 19: "Không để UI / MapMatcher
// / SpeedLimitManager tự ý mở/đóng file SD" — nothing else in the firmware
// opens an SD file directly). SpeedLimitManager.cpp is the only caller.
// Plain functions with an `sdMgr` prefix, not a class — same convention
// every other module in this project uses (gnssTaskStart(),
// webPortalInit()...), kept consistent rather than introducing this
// codebase's first namespace/singleton-object for what's really just one
// more owned-resource module like the others.
//
// Was "not thread-safe by design, only ever called from SpeedLimitManager's
// own task" until log/TripLogger.cpp (added 2026-09-16) became a second real
// caller running on its OWN FreeRTOS task. SD_MMC is one physical peripheral
// shared by both, so every function below is now internally mutex-protected
// (see SdCardManager.cpp's SdLock) — callers still don't need to think about
// locking, same as before, it's just no longer actually safe to skip.

// Mounts the card and validates metadata.bin (magic, format version,
// index.bin CRC) — does NOT read tiles yet (that's lazy, per-lookup, via
// sdMgrReadTile()). Returns false for anything that stops the map feature
// from working: no card, wrong format version, bad CRC, missing files.
// Never crashes or blocks indefinitely on a bad card — every failure path
// here is a plain `return false` plus one `Serial.println`, and the caller
// falls back to reporting UNKNOWN (spec sections 31, 35, 36).
bool sdMgrMount();

bool sdMgrIsAvailable(); // true only after a successful sdMgrMount()

// Fills `out` with the already-validated metadata (cheap: sdMgrMount()
// already read it into a static buffer). Returns false if !sdMgrIsAvailable().
bool sdMgrGetMetadata(SpeedMapMetadata &out);

// Points `*out` at the in-RAM TileIndexEntry array loaded once at
// sdMgrMount() time (spec's own tile_count for a single test region is
// small — see SpeedLimitManager.cpp for the actual cap) and sets *outCount.
// Returns false if !sdMgrIsAvailable().
bool sdMgrGetIndex(const TileIndexEntry **out, int *outCount);

// Finds a TileIndexEntry by tileId, supporting both PSRAM array and on-demand file index.
bool sdMgrFindTileEntry(uint32_t tileId, TileIndexEntry *outEntry);

// Points `*out` at the in-RAM CameraPoint array loaded once at
// sdMgrMount() time and sets *outCount — 0 (not a failure) when the card's
// /speedmap/cameras.bin is missing or empty, since camera data is OPTIONAL
// (see CameraPoint's own SpeedMapFormat.h comment): an older card built
// before this feature existed still works for everything else. Unlike
// sdMgrGetIndex()'s TileIndexEntry array, this doesn't require
// sdMgrIsAvailable() (a full, validated speedmap) — cameras.bin is loaded
// independently of metadata.bin/index.bin's own validation, so a region
// extract that for whatever reason has cameras but no tiles (or vice
// versa) still gets whatever it does have.
bool sdMgrGetCameras(const CameraPoint **out, int *outCount);

// Points `*out` at the in-RAM TrafficSignPoint array loaded at boot from /speedmap/signs.bin
bool sdMgrGetSigns(const TrafficSignPoint **out, int *outCount);

// Returns the street name for a given segment id (e.g. "Đ. Nguyễn Trãi", "QL 1A")
// or empty string if not available.
const char *sdMgrGetSegmentRoadName(uint32_t segId);

// Returns the street name for a given nameId (1-based index)
const char *sdMgrGetRoadName(uint16_t nameId);

// Reads one tile's segments out of the single packed /speedmap/tiles.bin
// blob (format V2, 2026-09-16 — see SpeedMapFormat.h's own history note on
// why V1's one-file-per-tile layout was abandoned) at `entry.fileOffset`,
// `entry.fileSize` bytes, into `outBuf` (caller-owned, `maxSegments`
// capacity). Returns how many RoadSegment records were actually read via
// `outCount`. The caller (SpeedLimitManager.cpp's getOrLoadTile()) already
// has `entry` from its own index lookup, so this never re-searches the
// index. Returns false if the seek/read fails — SpeedLimitManager.cpp
// treats that tile as empty (no candidates), not as a fatal error.

// Reads raw bytes from any file on SD with mutex protection
bool sdMgrReadBytes(const char *path, uint32_t offset, uint8_t *outBuf, size_t len);

bool sdMgrReadTile(const TileIndexEntry &entry, RoadSegment *outBuf, int maxSegments, int *outCount);

// Appends one line (a trailing '\n' is added) to a text file, creating the
// file and any missing parent directory if needed. Added 2026-09-16 for
// log/TripLogger.cpp — the trip-log CSV is the one thing in this project
// that writes to the SD card rather than only reading it, so this stays
// here (this module's sole-SD-owner rule, see the header comment above)
// instead of TripLogger.cpp opening its own File handle. Works even when
// sdMgrIsAvailable() is false (no speedmap database on the card, or
// sdMgrMount() hasn't run yet / failed its metadata.bin validation) — this
// lazily brings up the underlying SD_MMC peripheral itself on first use if
// nothing has yet (see SdCardManager.cpp's ensureSdMmcBegun()), so
// TripLogger's own task doesn't need to race SpeedLimitManager's task to
// mount the card first. Only actually fails if there's no card / it can't
// be brought up at all, or the file itself can't be opened for append.
bool sdMgrAppendLine(const char *path, const char *line);

// Dev-only diagnostic utility (added 2026-09-21 to retrieve and analyze a
// real road-test's trip log): streams an existing file's raw contents to
// Serial, wrapped in machine-parseable BEGIN/END markers, so a trip-log CSV
// can be pulled over the same serial link the rest of this project's dev
// loop already uses instead of physically pulling the microSD card. No
// automatic caller in the shipped firmware — invoke ad hoc (a temporary
// call from main_ui_demo.cpp, same pattern as this project's other
// TEMP-verification hacks) whenever a future test drive's log needs
// retrieving the same way.
bool sdMgrDumpFileToSerial(const char *path);

// Same dev-only-utility status as sdMgrDumpFileToSerial() above — prints
// one summary line (size, line count, max ego speed seen, whether any
// active target/event was ever logged) for a whole range of trip-log
// session files, so the real road-test session can be picked out from
// among many short/stationary bench-test sessions without dumping each
// one's full content first.
void sdMgrSummarizeTripLogs(uint32_t firstSessionId, uint32_t lastSessionId);

// Enumerates the trip-log session files actually present on the card into
// caller-owned arrays (ids and byte sizes, newest-last), returning how many
// were filled in — backs net/WebPortal.cpp's /triplog page so a finished
// drive's CSV can be listed and downloaded over the device's own WiFi AP
// instead of pulling the microSD card or dumping it over serial (added
// 2026-09-21 after doing exactly that the hard way).
int sdMgrListTripLogs(uint32_t *outIds, uint32_t *outSizes, int maxCount);

// Reads up to bufSize bytes of `path` starting at `offset` into `buf`,
// returning the byte count (0 at EOF, -1 if the file can't be opened).
// Chunked-with-offset rather than handing a File handle back to the
// caller: this module's whole point is that nothing else opens SD files
// directly (and its mutex is held only for the duration of each call, never
// across a whole slow HTTP response).
int sdMgrReadFileChunk(const char *path, size_t offset, uint8_t *buf, size_t bufSize);
