#pragma once
#include <stdint.h>

// Owns the real u-blox M10N GNSS on UART2 (see include/pincfg.h for the
// confirmed pin wiring) and publishes into core/SharedState.h's
// GnssSnapshot — the same contract SimTask used to fake (spec Phase 3:
// UART + NMEA parser + fix status + speed + 10Hz + filter).
//
// Runs on its own FreeRTOS task (Core 0), same pattern as TouchTask/LD2451.
// SimTask.h originally faked both radar and GNSS in one task; GNSS split out
// into this file 2026-09-15 once real M10N hardware got wired, and radar
// followed 2026-09-16 into radar/LD2451.h/.cpp once the real HLK-LD2451 got
// wired too — SimTask is now fully retired (see its own header).
void gnssTaskStart();

// Shared "is this speed reading real motion, or just GPS noise on a parked
// vehicle" cutoff — GNSS.cpp uses it both to gate course/heading validity
// (a stationary GPS's course is meaningless) and to track how long the
// vehicle has been stationary (see gnssMsSinceStationary() below);
// Dashboard.cpp reuses the same constant to auto-wake a dimmed screen the
// instant real movement resumes, rather than inventing a second number for
// what's really the same underlying idea.
static const float kGnssMotionThresholdKmh = 3.0f;

// How long gnss/GNSS.cpp will keep reporting a HELD (dead-reckoned) heading
// after the module's own live course last went invalid — added 2026-09-22
// ("dự đoán hướng di chuyển của xe") to bridge the brief GPS-course gaps
// that happen exactly while slowing through an intersection or crossing an
// overpass (speed dips below kGnssMotionThresholdKmh above, or the module's
// own course fix briefly glitches), which were costing
// map/SpeedLimitManager.cpp its only way to tell the road ahead from a
// crossing street right at the moment it needed to. ~6s comfortably covers
// a slow-and-go through a junction without holding a stale heading long
// enough to matter if the car has genuinely stopped or turned in place.
static const uint32_t kHeadingHoldMs = 6000;

// How long the vehicle has been continuously stationary, in milliseconds —
// ui/Dashboard.cpp's auto-dim gate (user-requested 2026-09-16, "che do tu
// giam do sang man hinh chi duoc thuc hien khi xe khong chuyen dong sau 3
// phut") reads this instead of touch-idle time, so the screen stays at full
// brightness while actually driving even if the driver never touches it,
// and only dims once genuinely parked/stopped for a while. Resets to 0
// (i.e. "moving", conservatively) whenever speed is above the noise floor
// OR there's no reliable fix at all — never assume stationary from an
// absence of information.
uint32_t gnssMsSinceStationary();
