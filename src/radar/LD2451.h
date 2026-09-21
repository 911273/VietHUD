#pragma once

// Owns the real HLK-LD2451 24GHz radar on UART1 (see include/pincfg.h for
// the confirmed pin wiring) and publishes into core/SharedState.h's
// RadarSnapshot — the same contract SimTask used to fake (spec Phase 2:
// UART + protocol parser + raw target display + multi-target + max 5).
//
// Protocol source: Hi-Link's official "HLK-LD2451 Serial communication
// protocol V1.03" datasheet (Shenzhen Hi-Link Electronic Co., Ltd,
// 2024-7-1), fetched directly from the manufacturer
// (d.hlktech.net/download/HLK-LD2451/1/...) 2026-09-16 — not guessed, per
// spec section 4.2's explicit warning not to invent the frame protocol.
//
// Runs on its own FreeRTOS task (Core 0), same pattern as GNSS.cpp/
// TouchTask.cpp. Now that this is real hardware, radar/SimTask.h's
// simulated radar is fully retired (SimTask already stopped faking GNSS
// when that became real 2026-09-15) — main_ui_demo.cpp no longer starts
// it.
void radarTaskStart();
