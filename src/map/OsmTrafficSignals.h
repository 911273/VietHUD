#pragma once
#include <stdint.h>

// OpenStreetMap traffic signals (highway=traffic_signals) for Vietnam, compiled
// into flash by tools/map_builder/build_osm_signals.py. Merged into the traffic-
// light warnings at boot by SdCardManager.cpp (skipping any light the card's
// signs.bin already has within 30 m) — the WYN alert data has no traffic lights
// in inner Hanoi. Data (c) OpenStreetMap contributors, ODbL 1.0.
struct OsmSignalPoint {
    int32_t latE7;
    int32_t lonE7;
    uint16_t directionDeg; // travel direction it controls (traffic_signals:direction); 0xFFFF = any approach
};
extern const int kOsmTrafficSignalCount;
extern const OsmSignalPoint kOsmTrafficSignals[];
