#pragma once
#include "SpeedMapFormat.h"

// The map-matching engine (spec section 18's "SpeedLimitManager" module) —
// owns the tile cache, runs the distance+heading+direction+confidence+
// hysteresis matching algorithm described in
// C:\Users\phamq\.claude\plans\wobbly-swinging-chipmunk.md, and is the ONLY
// thing that calls into SdCardManager.h (spec section 19). Publishes
// results into core/SharedState.h's RoadInfoSnapshot, same publish/snapshot
// pattern gnss/GNSS.cpp already uses — ui/Dashboard.cpp,
// ui/Settings.cpp and net/WebPortal.cpp all just read roadInfoSnapshot(),
// never touch this module or the SD card directly.
//
// Runs on its own FreeRTOS task (Core 0). At boot: mounts the SD card,
// validates the database, and — if the database provides a self-test point
// (SpeedMapMetadata's testLatE7/testLonE7/testRoadId) — runs one match
// against it and logs PASS/FAIL, so the whole SD -> index -> tile -> match
// pipeline can be verified from the serial log without a live GNSS fix
// (this project has not seen one in any session yet).
void speedLimitManagerStart();

// Dynamic warning distance based on vehicle speed:
// Minimum 100m. Scales smoothly with vehicle speed (lead time ~14-15s: ~4.5m per km/h).
float computeDynamicWarnDistance(float speedKmh);

// How far ahead regulatory SIGNS (resident area, no-overtaking, toll booth,
// traffic light) start being reported in RoadInfoSnapshot. Lives in the
// header, unlike the matcher's other tuning constants, because
// ui/Dashboard.cpp needs it too: its alert card scales the countdown digits,
// progress bar and border by remaining distance as a fraction of the alert's
// own full warning range, and hardcoding 350 a second time in the UI is
// exactly how the two would eventually drift apart. Deliberately NOT
// user-tunable the way cfg.aheadLimitWarnDistM/cfg.cameraWarnDistM are —
// giving all six alert types their own slider would clutter Settings for
// little gain when they're all in the same 300-400m ballpark.
constexpr float kSignWarnDistanceM = 350.0f;

// Read-only access to the already-loaded database metadata (region, map
// version, attribution...) for Settings.cpp's Speed Map diagnostics group
// and WebPortal.cpp's /api/speedmap/debug — does NOT touch the SD card
// itself (SdCardManager.h stays the sole SD owner per spec section 19),
// just returns the copy SpeedLimitManager.cpp already read at boot. Returns
// false if no database is loaded (SD missing, bad format, etc — the same
// cases RoadInfoSnapshot.mapLoaded is false for).
bool speedLimitManagerGetInfo(SpeedMapMetadata &out);

// Human-readable SpeedSource name, for Settings.cpp/WebPortal.cpp diagnostic
// display — kept here rather than as a method on RoadInfoSnapshot since
// SharedState.h deliberately doesn't depend on SpeedMapFormat.h's enums
// (it's a generic pub/sub hub, not map-specific).
const char *speedSourceStr(uint8_t source);

// Copies every RoadSegment within `radiusM` of (lat,lon) into `out` (up to
// `maxOut`), for map/MapRenderer.cpp's live background map (user-requested
// 2026-09-22, "tich hop ban do vector offline lam nen"). Reuses THIS
// module's own already-warm tile cache (the same 5-tile LRU cache the live
// map-matching above maintains) rather than making MapRenderer.cpp open a
// second, independent path into SdCardManager.h — SdCardManager.h's own
// file comment states this module is meant to be its one and only caller.
// Returns false if no database is loaded (same case RoadInfoSnapshot's
// mapLoaded is false for) — MapRenderer.cpp treats that as "nothing to
// draw", not an error, same honesty convention every other sensor here
// follows before real data exists for it.
bool speedLimitManagerGetNearbySegments(float lat, float lon, float radiusM, RoadSegment *out, int maxOut,
                                         int *outCount);

// One camera or sign near the car, for the live background map's marker
// dots — `kind` values match map/MapRenderer.h's own MapMarkerKind so
// MapRenderer.cpp can pass it straight through into the screen-space
// MapMarker it publishes, without SharedState.h needing to depend on this
// header's enums (same reasoning RoadInfoSnapshot::source's own comment
// gives for keeping enum values out of the generic pub/sub struct itself).
struct NearbyMarkerRaw {
    float lat, lon;
    uint8_t kind;
};

// Same "reuse this module's own already-loaded data, don't make the caller
// open a second path into SdCardManager.h" reasoning as
// speedLimitManagerGetNearbySegments() above — cameras.bin/signs.bin are
// flat, untiled arrays already loaded whole into RAM at boot
// (SdCardManager.h's sdMgrGetCameras()/sdMgrGetSigns()), so this is a plain
// linear distance-filter scan over both, not a second tile cache.
bool speedLimitManagerGetNearbyMarkers(float lat, float lon, float radiusM, NearbyMarkerRaw *out, int maxOut,
                                        int *outCount);
