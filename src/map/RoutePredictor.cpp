#include "RoutePredictor.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------
// Small flat-earth geometry helpers, local to this module (kept here so
// RoutePredictor has no dependency on SpeedLimitManager.cpp's own copies and
// stays unit-testable standalone). Same equirectangular approximation, same
// 110540 / 111320 m-per-degree constants the rest of map/ uses.
// ---------------------------------------------------------------------
namespace {

struct Vec2 {
    float x, y;
};

// Offset of (lat,lon) from (originLat,originLon) in local meters.
Vec2 toLocalM(float latDeg, float lonDeg, float originLatDeg, float originLonDeg) {
    float dLat = latDeg - originLatDeg;
    float dLon = lonDeg - originLonDeg;
    float y = dLat * 110540.0f;
    float x = dLon * 111320.0f * cosf(originLatDeg * (float)M_PI / 180.0f);
    return {x, y};
}

// Smallest absolute angle between two bearings, 0..180.
float angDiff(float a, float b) {
    float d = fmodf(fabsf(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

} // namespace

void Route::reset() {
    count_ = 0;
    forkAtM_ = 1e9f;
    originLat_ = 0;
    originLon_ = 0;
}

float Route::totalLenM() const {
    if (count_ == 0) return 0.0f;
    return segs_[count_ - 1].startDistM + segs_[count_ - 1].lenM;
}

// Rank of a road class for continuity: 1 major < 2 main < 3 small; 0 (other/
// unclassified) counts as small.
static int classRank(uint8_t c) { return (c >= 1 && c <= 3) ? c : 3; }

// Cost of continuing from the incoming road onto a candidate branch at a node
// (degrees-equivalent; lowest wins). Pure geometry picks a straight side street
// over a main road that bends — drivers overwhelmingly stay on the bigger road,
// stay off exit ramps, and can't change level (flyover <-> road below) except
// via a ramp.
static float branchCost(float turnDeg, uint8_t inClass, uint8_t inFlags, uint8_t cClass, uint8_t cFlags) {
    float cost = turnDeg;
    int drop = classRank(cClass) - classRank(inClass);
    if (drop > 0) cost += 25.0f * drop; // onto a smaller road
    if ((cFlags & SEGFLAG_LINK) && !(inFlags & SEGFLAG_LINK)) cost += 20.0f; // taking an exit/entry ramp
    bool inBridge = (inFlags & SEGFLAG_BRIDGE) != 0, cBridge = (cFlags & SEGFLAG_BRIDGE) != 0;
    if (inBridge != cBridge && !((cFlags | inFlags) & SEGFLAG_LINK)) cost += 30.0f; // level change w/o ramp
    return cost;
}

int Route::build(const RoadSegment &startSeg, float startHeadingDeg, float aheadM, SegmentProviderFn provider,
                 void *ctx) {
    reset();
    if (aheadM <= 0.0f) return 0;

    // Orient the first segment to the direction of travel: if the car's heading
    // is closer to start->end, walk it forward; else walk it backward (segment
    // geometry is undirected on a bidirectional road; a one-way record still
    // gets oriented to its own legal direction below via direction()).
    int32_t sLat = startSeg.startLatE7, sLon = startSeg.startLonE7;
    int32_t eLat = startSeg.endLatE7, eLon = startSeg.endLonE7;
    float segHeading = (float)startSeg.headingDeg; // bearing start->end
    bool reversed = false;
    if (angDiff(startHeadingDeg, fmodf(segHeading + 180.0f, 360.0f)) < angDiff(startHeadingDeg, segHeading)) {
        reversed = true;
    }
    // Honor a legal one-way: a DIR_FORWARD record can only be traveled
    // start->end, DIR_BACKWARD only end->start, regardless of which way the
    // heading looked. (Most segments are BIDIRECTIONAL, where heading wins.)
    if (startSeg.direction == DIR_FORWARD) reversed = false;
    else if (startSeg.direction == DIR_BACKWARD) reversed = true;

    if (reversed) {
        sLat = startSeg.endLatE7; sLon = startSeg.endLonE7;
        eLat = startSeg.startLatE7; eLon = startSeg.startLonE7;
        segHeading = fmodf(segHeading + 180.0f, 360.0f);
    }

    originLat_ = sLat / 1e7f;
    originLon_ = sLon / 1e7f;

    uint32_t visited[kMaxSegments];
    int visitedCount = 0;
    float arcLen = 0;

    int32_t nodeLat = sLat, nodeLon = sLon; // chain node = END of the last appended segment
    float incomingHeading = segHeading;
    uint8_t incomingClass = startSeg.roadClass, incomingFlags = startSeg.flags;
    bool firstSeg = true;

    for (int step = 0; step < kMaxSegments; step++) {
        RoadSegment pick;
        bool havePick = false;
        float pickHeading = 0;
        int32_t pickStartLat = 0, pickStartLon = 0, pickEndLat = 0, pickEndLon = 0;

        if (firstSeg) {
            pick = startSeg;
            pickStartLat = sLat; pickStartLon = sLon;
            pickEndLat = eLat; pickEndLon = eLon;
            pickHeading = segHeading;
            havePick = true;
            firstSeg = false;
            nodeLat = eLat; nodeLon = eLon; // next chain node = this segment's end
        } else {
            // Find segments incident to the current node and pick the one
            // continuing straightest. The provider returns RAW (unoriented)
            // segments; we orient each to start at the shared node.
            RoadSegment cand[kMaxNodeCandidates];
            int ncand = provider ? provider(nodeLat, nodeLon, cand, kMaxNodeCandidates, ctx) : 0;
            float bestTurn = 1e9f; // lowest branch COST (turn angle + penalties, see branchCost)
            int plausible = 0;     // branches continuing within kForkTurnDeg (fork detection)
            for (int c = 0; c < ncand; c++) {
                const RoadSegment &seg = cand[c];
                // Skip anything already on the route (prevents U-turns/loops).
                bool seen = false;
                for (int v = 0; v < visitedCount; v++) {
                    if (visited[v] == seg.id) { seen = true; break; }
                }
                if (seen) continue;

                // Tolerant node match (not exact ==) so a route stitches across
                // tile boundaries when the map is built from tile-clipped vector
                // data — see segNodesCoincide() in SpeedMapFormat.h.
                bool startsHere = segNodesCoincide(seg.startLatE7, seg.startLonE7, nodeLat, nodeLon);
                bool endsHere = segNodesCoincide(seg.endLatE7, seg.endLonE7, nodeLat, nodeLon);
                if (!startsHere && !endsHere) continue;

                // Determine the orientation we'd travel this candidate, and
                // reject orientations a legal one-way forbids.
                bool travelFwd; // travel start->end?
                if (seg.direction == DIR_FORWARD) travelFwd = true;
                else if (seg.direction == DIR_BACKWARD) travelFwd = false;
                else travelFwd = startsHere ? true : (endsHere ? false : true);
                // If it both starts and ends here (zero-length/degenerate), skip.
                if (startsHere && endsHere) continue;

                int32_t cStartLat, cStartLon, cEndLat, cEndLon;
                float cHeading;
                if (travelFwd) {
                    cStartLat = seg.startLatE7; cStartLon = seg.startLonE7;
                    cEndLat = seg.endLatE7; cEndLon = seg.endLonE7;
                    cHeading = (float)seg.headingDeg;
                } else {
                    cStartLat = seg.endLatE7; cStartLon = seg.endLonE7;
                    cEndLat = seg.startLatE7; cEndLon = seg.startLonE7;
                    cHeading = fmodf((float)seg.headingDeg + 180.0f, 360.0f);
                }
                // The chosen orientation must actually leave from the shared
                // node (cStart ~= node). If a one-way forces an orientation
                // that doesn't, this candidate is unusable here. Tolerant match,
                // same reason as startsHere/endsHere above.
                if (!segNodesCoincide(cStartLat, cStartLon, nodeLat, nodeLon)) continue;

                float turn = angDiff(incomingHeading, cHeading);
                if (turn <= kForkTurnDeg) plausible++;
                float cost = branchCost(turn, incomingClass, incomingFlags, seg.roadClass, seg.flags);
                if (cost < bestTurn) {
                    bestTurn = cost;
                    pick = seg;
                    pickStartLat = cStartLat; pickStartLon = cStartLon;
                    pickEndLat = cEndLat; pickEndLon = cEndLon;
                    pickHeading = cHeading;
                    havePick = true;
                }
            }
            if (!havePick) break; // dead end (cul-de-sac) — route ends here, which is fine
            if (plausible >= 2 && forkAtM_ > 1e8f) forkAtM_ = arcLen; // first fork: path beyond is a guess
            nodeLat = pickEndLat; nodeLon = pickEndLon;
        }

        // Compute length and append.
        Vec2 a = toLocalM(pickStartLat / 1e7f, pickStartLon / 1e7f, originLat_, originLon_);
        Vec2 b = toLocalM(pickEndLat / 1e7f, pickEndLon / 1e7f, originLat_, originLon_);
        float lenM = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));

        RouteSeg &rs = segs_[count_];
        rs.id = pick.id;
        rs.startLatE7 = pickStartLat; rs.startLonE7 = pickStartLon;
        rs.endLatE7 = pickEndLat; rs.endLonE7 = pickEndLon;
        rs.speedLimitKmh = pick.speedLimitKmh;
        rs.roadClass = pick.roadClass;
        rs.flags = pick.flags;
        rs.headingDeg = pickHeading;
        rs.lenM = lenM;
        rs.startDistM = arcLen;
        arcLen += lenM;
        visited[visitedCount++] = pick.id;
        count_++;
        incomingHeading = pickHeading;
        incomingClass = pick.roadClass;
        incomingFlags = pick.flags;

        if (arcLen >= aheadM) break;
    }
    return count_;
}

bool Route::project(float latDeg, float lonDeg, float lateralTolM, float *outDistM, float *outLat, float *outLon,
                    float *outHeadingDeg, float *outLateralM) const {
    if (count_ == 0) return false;
    float bestPerp = 1e9f, bestDist = 0, bestLat = 0, bestLon = 0, bestHeading = 0;
    bool found = false;
    for (int i = 0; i < count_; i++) {
        const RouteSeg &s = segs_[i];
        float slat = s.startLatE7 / 1e7f, slon = s.startLonE7 / 1e7f;
        float elat = s.endLatE7 / 1e7f, elon = s.endLonE7 / 1e7f;
        Vec2 p = toLocalM(latDeg, lonDeg, slat, slon);
        Vec2 e = toLocalM(elat, elon, slat, slon);
        float segLenSq = e.x * e.x + e.y * e.y;
        float t = segLenSq > 1e-4f ? (p.x * e.x + p.y * e.y) / segLenSq : 0.0f;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float perpX = p.x - e.x * t, perpY = p.y - e.y * t;
        float perp = sqrtf(perpX * perpX + perpY * perpY);
        if (perp < bestPerp) {
            bestPerp = perp;
            bestDist = s.startDistM + t * s.lenM;
            bestLat = slat + t * (elat - slat);
            bestLon = slon + t * (elon - slon);
            bestHeading = s.headingDeg;
            found = true;
        }
    }
    if (!found || bestPerp > lateralTolM || bestDist < 0) return false;
    if (outDistM) *outDistM = bestDist;
    if (outLat) *outLat = bestLat;
    if (outLon) *outLon = bestLon;
    if (outHeadingDeg) *outHeadingDeg = bestHeading;
    if (outLateralM) *outLateralM = bestPerp;
    return true;
}

bool Route::limitChangeAhead(float fromDistM, float currentLimitKmh, float maxM, float &outDistM,
                             float &outLimitKmh) const {
    for (int i = 0; i < count_; i++) {
        const RouteSeg &s = segs_[i];
        float segEnd = s.startDistM + s.lenM;
        if (segEnd <= fromDistM) continue;          // wholly behind the car
        if (s.startDistM - fromDistM > maxM) break;  // wholly beyond the warn horizon
        if (s.speedLimitKmh < 0) continue;           // unknown tag — not evidence of a change
        float lim = (float)s.speedLimitKmh;
        if (lim != currentLimitKmh) {
            float d = s.startDistM - fromDistM;
            if (d < 0) d = 0;
            outDistM = d;
            outLimitKmh = lim;
            return true;
        }
    }
    return false;
}

bool Route::limitAt(float distM, float &outLimitKmh) const {
    for (int i = 0; i < count_; i++) {
        const RouteSeg &s = segs_[i];
        if (distM >= s.startDistM && distM <= s.startDistM + s.lenM && s.speedLimitKmh >= 0) {
            outLimitKmh = (float)s.speedLimitKmh;
            return true;
        }
    }
    // Past the built end (or on an untagged stretch): use the last known limit.
    if (count_ > 0 && distM > 0) {
        for (int i = count_ - 1; i >= 0; i--) {
            if (segs_[i].speedLimitKmh >= 0) {
                outLimitKmh = (float)segs_[i].speedLimitKmh;
                return true;
            }
        }
    }
    return false;
}

bool Route::containsSegment(uint32_t id) const {
    for (int i = 0; i < count_; i++)
        if (segs_[i].id == id) return true;
    return false;
}

float segPointDistM(const RoadSegment &seg, float latDeg, float lonDeg) {
    float slat = seg.startLatE7 / 1e7f, slon = seg.startLonE7 / 1e7f;
    Vec2 p = toLocalM(latDeg, lonDeg, slat, slon);
    Vec2 e = toLocalM(seg.endLatE7 / 1e7f, seg.endLonE7 / 1e7f, slat, slon);
    float L = e.x * e.x + e.y * e.y;
    float t = L > 1e-4f ? (p.x * e.x + p.y * e.y) / L : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float dx = p.x - e.x * t, dy = p.y - e.y * t;
    return sqrtf(dx * dx + dy * dy);
}

bool Route::ownsPoint(float latDeg, float lonDeg, float maxLateralM, const uint32_t *nearIds, const float *nearDistM,
                      int nNear, float marginM, float *outDistM, float *outHeadingDeg) const {
    float dist, lateral, heading;
    if (!project(latDeg, lonDeg, maxLateralM, &dist, 0, 0, &heading, &lateral)) return false;
    for (int i = 0; i < nNear; i++) {
        if (containsSegment(nearIds[i])) continue;
        if (nearDistM[i] + marginM < lateral) return false; // clearly on another road
    }
    if (outDistM) *outDistM = dist;
    if (outHeadingDeg) *outHeadingDeg = heading;
    return true;
}
