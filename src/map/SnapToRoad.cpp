#include "SnapToRoad.h"
#include <Arduino.h>
#include <math.h>

static float angularDiffDeg(float a, float b) {
    float d = fmodf(fabsf(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

SnapToRoad::SnapToRoad() : trailCount(0), lastTrailUpdateMs(0), smoothedHeading(0.0f) {
    currentPos.latDeg = 0.0f;
    currentPos.lonDeg = 0.0f;
    currentPos.headingDeg = 0.0f;
    currentPos.offsetDistM = 0.0f;
    currentPos.isSnapped = false;
}

// Appends the CURRENT currentPos to the trail if the vehicle is moving and
// enough time has passed — factored out so both the "matched a road" path
// below and the "no road matched" early-return can record a breadcrumb.
// Previously this only ran in the matched-road path, so the trail silently
// stopped recording anytime the road briefly didn't resolve — exactly the
// ambiguous-junction/overpass case this project's map-matching already
// struggles with — leaving a gap in the trail right when the driver would
// most want to see where they'd been. Found 2026-09-22 reviewing the whole
// matching pipeline.
void SnapToRoad::recordTrailPoint(float speedKmh) {
    uint32_t now = millis();
    if (speedKmh > 3.0f && (now - lastTrailUpdateMs > 400)) {
        lastTrailUpdateMs = now;
        if (trailCount < MAX_TRAIL_POINTS) {
            trail[trailCount].latDeg = currentPos.latDeg;
            trail[trailCount].lonDeg = currentPos.lonDeg;
            trailCount++;
        } else {
            for (int i = 0; i < MAX_TRAIL_POINTS - 1; i++) {
                trail[i] = trail[i + 1];
            }
            trail[MAX_TRAIL_POINTS - 1].latDeg = currentPos.latDeg;
            trail[MAX_TRAIL_POINTS - 1].lonDeg = currentPos.lonDeg;
        }
    }
}

SnappedPosition SnapToRoad::update(float rawLat, float rawLon, float rawHeading, float speedKmh,
                                  bool hasMatchedRoad, const RoadSegment *matchedSeg) {
    if (!hasMatchedRoad || !matchedSeg) {
        currentPos.latDeg = rawLat;
        currentPos.lonDeg = rawLon;
        currentPos.headingDeg = rawHeading;
        currentPos.offsetDistM = 0.0f;
        currentPos.isSnapped = false;
        recordTrailPoint(speedKmh);
        return currentPos;
    }

    float startLat = matchedSeg->startLatE7 / 1e7f;
    float startLon = matchedSeg->startLonE7 / 1e7f;
    float endLat = matchedSeg->endLatE7 / 1e7f;
    float endLon = matchedSeg->endLonE7 / 1e7f;

    float cosLat = cosf(startLat * (float)M_PI / 180.0f);
    float ey = (endLat - startLat) * 110540.0f;
    float ex = (endLon - startLon) * 111320.0f * cosLat;
    float py = (rawLat - startLat) * 110540.0f;
    float px = (rawLon - startLon) * 111320.0f * cosLat;

    float segLenSq = ex * ex + ey * ey;
    float t = segLenSq > 1e-4f ? (px * ex + py * ey) / segLenSq : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float snapLat = startLat + t * (endLat - startLat);
    float snapLon = startLon + t * (endLon - startLon);

    float dx = px - ex * t;
    float dy = py - ey * t;
    float offsetDist = sqrtf(dx * dx + dy * dy);

    float roadHeading = (float)matchedSeg->headingDeg;
    float headingDiff = angularDiffDeg(rawHeading, roadHeading);

    // If bidirectional road, check reverse heading direction
    if (!(matchedSeg->flags & 0x01)) { // SEGFLAG_ONEWAY = 0x01
        float reverseHeading = fmodf(roadHeading + 180.0f, 360.0f);
        float revDiff = angularDiffDeg(rawHeading, reverseHeading);
        if (revDiff < headingDiff) {
            roadHeading = reverseHeading;
            headingDiff = revDiff;
        }
    }

    // Snapping threshold: up to 22m and heading within 65 deg (covers curve entries)
    bool canSnap = (offsetDist <= 22.0f && headingDiff <= 65.0f);

    if (canSnap) {
        currentPos.latDeg = snapLat;
        currentPos.lonDeg = snapLon;
        // Smooth heading towards road heading
        float diff = fmodf(roadHeading - smoothedHeading + 540.0f, 360.0f) - 180.0f;
        smoothedHeading = fmodf(smoothedHeading + diff * 0.40f + 360.0f, 360.0f);
        currentPos.headingDeg = smoothedHeading;
        currentPos.offsetDistM = offsetDist;
        currentPos.isSnapped = true;
    } else {
        // Fallback to raw GPS
        currentPos.latDeg = rawLat;
        currentPos.lonDeg = rawLon;
        currentPos.headingDeg = rawHeading;
        smoothedHeading = rawHeading;
        currentPos.offsetDistM = offsetDist;
        currentPos.isSnapped = false;
    }

    recordTrailPoint(speedKmh);

    return currentPos;
}

int SnapToRoad::getTrail(const TrailPoint **outPoints) const {
    if (outPoints) *outPoints = trail;
    return trailCount;
}
