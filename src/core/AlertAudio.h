#pragma once
#include "core/SharedState.h"

// Driving-alert sounds, shared by every model's UI (see AlertAudio.cpp).
// Call once per UI refresh (~150 ms) with the snapshots that refresh used.
void alertAudioTick(const GnssSnapshot &gnss, const RoadInfoSnapshot &road, bool speeding);

// Over the matched limit + cfg.overspeedOffsetKmh, with a real fix.
bool alertIsSpeeding(const GnssSnapshot &gnss, const RoadInfoSnapshot &road);
