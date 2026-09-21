#pragma once

// Starts the simulated radar data task (spec section 7-12 logic, moved out
// of the old monolithic main_ui_demo.cpp). Runs on Core 0, independent of
// the UI/render task on Core 1, publishing into core/SharedState.h.
//
// Used to also fake GNSS (radar+GNSS in one task, since both were synthetic
// with no real per-peripheral timing to respect) — split out 2026-09-15 once
// real GNSS M10N hardware got wired, into gnss/GNSS.h/.cpp with its own
// task/timing, exactly as this header used to say would happen.
//
// RETIRED 2026-09-16: real HLK-LD2451 hardware got wired too, replacing
// this with radar/LD2451.h/.cpp — main_ui_demo.cpp no longer calls
// simTaskStart(). Kept in the tree for reference (the per-track motion
// profiles were useful for exercising the UI before any real sensor
// existed) but not part of the build path anymore.
void simTaskStart();
