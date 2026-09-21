#pragma once

// Lightweight per-session driving log to the microSD card, added 2026-09-16
// per docs/V1.2_hardening_proposal.md section G's "SD/LittleFS logging"
// row — originally for tuning TTC/distance/confidence thresholds against a
// real drive; now (radar removed 2026-09-21) logs GNSS speed plus
// speed-limit/camera/sign warning state instead, same "review a real drive
// afterward instead of only road-testing by feel" purpose.
//
// Writes go through map/SdCardManager.h's sdMgrAppendLine() (this module
// never opens an SD file directly — that header's sole-SD-owner rule, now
// mutex-protected since this is its second real caller task alongside
// map/SpeedLimitManager.cpp). Runs on its own FreeRTOS task (Core 0), same
// pattern as every other subsystem here.
//
// One CSV file per boot session, named by an NVS-persisted incrementing
// counter (see TripLogger.cpp) rather than GNSS date — spec section 15.4
// already establishes that GNSS date/time can't be trusted before a fix,
// and this needs a stable filename from the moment the task starts, not
// only once a fix eventually arrives. A periodic ~1 Hz sample (ego speed,
// speed-limit match, camera/sign-ahead state) lands in it for reviewing
// overall trip shape afterward.
//
// Fails open like every other optional subsystem in this project: with no
// SD card available or cfg.tripLoggingEnabled off, this task simply does
// nothing each tick — it never blocks or affects the GNSS/UI tasks.
void tripLoggerStart();

// Human-readable esp_reset_reason(), shared (added 2026-09-21) rather than
// duplicated: this module writes it as the first line of every session's
// CSV so a reset that happens mid-drive — with no serial monitor attached
// in a real car — is still diagnosable afterward, and main_ui_demo.cpp
// prints the same string at boot for the tethered-bench case. One source
// of truth for the mapping, two consumers.
const char *tripLogResetReasonStr();
