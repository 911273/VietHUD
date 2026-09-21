#pragma once

// RETIRED 2026-09-15: this was the first-pass speed-limit lookup (a plain
// "/speedlimits.csv" of lat,lon,radius_m,speed_limit_kmh on the SD card,
// nearest-point-within-radius matching, no heading/direction awareness).
// The user replaced it same-day with a full offline map-matching
// specification (binary tile database built offline by a PC tool, real
// heading/direction-aware matching with confidence + hysteresis) — see
// map/SpeedMapFormat.h, map/SdCardManager.h, and map/SpeedLimitManager.h for
// the real implementation, and
// C:\Users\phamq\.claude\plans\wobbly-swinging-chipmunk.md for the spec.
// main_ui_demo.cpp calls speedLimitManagerStart() now, not mapTaskStart().
// Kept in the tree for reference/history, same treatment radar/SimTask.h
// got when IT was superseded by real hardware — not part of the build path
// (removed from platformio.ini's build_src_filter along with .cpp).
void mapTaskStart();
