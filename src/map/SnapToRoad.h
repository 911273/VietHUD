#pragma once
#include "SpeedMapFormat.h"
#include <stdint.h>
#include <stdbool.h>

// Snap-to-Road Engine for VietHUD
// Projects raw GNSS fix orthogonally onto the matched road segment to eliminate
// GPS drift and keep the ego-vehicle centered on the road.

struct SnappedPosition {
    float latDeg;
    float lonDeg;
    float headingDeg;
    float offsetDistM;
    bool isSnapped;
};

struct TrailPoint {
    float latDeg;
    float lonDeg;
};

#define MAX_TRAIL_POINTS 30

class SnapToRoad {
public:
    static SnapToRoad &instance() {
        static SnapToRoad inst;
        return inst;
    }

    // Updates snapped position given raw GNSS and currently matched RoadSegment
    SnappedPosition update(float rawLat, float rawLon, float rawHeading, float speedKmh,
                           bool hasMatchedRoad, const RoadSegment *matchedSeg);

    // Trail breadcrumbs (recent vehicle positions)
    int getTrail(const TrailPoint **outPoints) const;

    const SnappedPosition &getPos() const { return currentPos; }

private:
    SnapToRoad();
    void recordTrailPoint(float speedKmh);
    SnappedPosition currentPos;
    TrailPoint trail[MAX_TRAIL_POINTS];
    int trailCount;
    uint32_t lastTrailUpdateMs;
    float smoothedHeading;
};
