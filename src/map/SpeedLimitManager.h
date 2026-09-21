#pragma once
#include "SpeedMapFormat.h"

// The map-matching engine (spec section 18's "SpeedLimitManager" module) —
// owns the tile cache, runs the distance+heading+direction+confidence+
// hysteresis matching algorithm described in
// C:\Users\phamq\.claude\plans\wobbly-swinging-chipmunk.md, and is the ONLY
// thing that calls into SdCardManager.h (spec section 19). Publishes
// results into core/SharedState.h's RoadInfoSnapshot, same publish/snapshot
// pattern radar/LD2451.cpp and gnss/GNSS.cpp already use — ui/Dashboard.cpp,
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
