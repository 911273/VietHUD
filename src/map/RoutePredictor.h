#pragma once
#include "SpeedMapFormat.h" // RoadSegment, RoadDirection
#include <stdint.h>

// =====================================================================
// RoutePredictor — forward-route engine (added 2026-09-23)
// ---------------------------------------------------------------------
// Builds a short FORWARD ROUTE polyline ahead of the car by chaining
// RoadSegments that share an exact endpoint node, then answers every
// "what's coming" question by walking that route BY ARC LENGTH instead of
// projecting a straight line off the GPS heading. This is the route-based
// replacement for the old point-based + heading-cone matching.
//
// WHY THIS EXISTS (the three real bugs it fixes):
//   1. A straight-line projection cuts corners — at a curve or junction it
//      lands on a parallel/cross road, so the "next speed limit" was for a
//      road the car isn't turning onto.
//   2. A bearing cone can't tell a sign on the car's own road from one on an
//      adjacent road heading the same way, and can't see a sign just around a
//      bend (off-cone) — so the "next" warning was often not the next one
//      actually reached.
//   3. Lose GPS in a tunnel and there was nothing to project from when it
//      returned, so the limit stayed blank until the car happened to drive
//      over a fresh matching segment/sign. A persisted route fixes this.
//
// DESIGN CONSTRAINTS (deliberate):
//   * RoutePredictor NEVER touches the SD card or the tile cache itself. It
//     sees road data ONLY through a SegmentProviderFn callback the caller
//     supplies (SpeedLimitManager backs it with its existing tile cache; a
//     host unit test backs it with a fixed in-memory array). This keeps the
//     module pure and unit-testable on a desktop, and honors "SpeedLimitManager
//     vẫn là thành phần quản lý việc lấy dữ liệu road/sign từ SD".
//   * No dynamic allocation: the route is a fixed-capacity array (kMaxSegments)
//     owned by the Route instance. Nothing mallocs in the realtime loop.
//   * No global/static state: Route is an instantiable object, so several can
//     exist (and a test can build many independent routes). SpeedLimitManager
//     owns exactly one.
//   * Bounded work: build() stops at min(kMaxSegments, aheadM) and the
//     provider caps how many candidates it returns per node.
//
// GEOMETRY: all distances use the same equirectangular flat-earth
// approximation the rest of map/ uses (good to a few cm over these ranges,
// far tighter than GNSS accuracy). Nodes are matched by EXACT int32 E7
// equality — confirmed valid on the real database (every segment's END node
// == some segment's START node; see the map-data notes).
// =====================================================================

// One segment of the built route, in travel order.
struct RouteSeg {
    uint32_t id;
    int32_t startLatE7, startLonE7; // start node (travel direction)
    int32_t endLatE7, endLonE7;     // end node (travel direction)
    int16_t speedLimitKmh;          // -1 = unknown
    uint8_t roadClass;              // RoadSegment::roadClass (1 major .. 3 small, 0 other)
    uint8_t flags;                  // SEGFLAG_* (bridge / tunnel / link)
    uint8_t source;                 // RoadSegment::speedSource (SPEED_SOURCE_*)
    float headingDeg;               // forward heading of THIS route direction (start->end as chained)
    float lenM;                     // segment length, meters
    float startDistM;               // arc length from route origin to this segment's start node
};

// Segment provider: given a node (E7 lat/lon), copy into `out` up to `maxOut`
// RoadSegments incident to that node (as either endpoint, RAW orientation —
// Route::build does the orienting), returning how many were written. `ctx` is
// an opaque caller pointer. This is RoutePredictor's ONLY view of road data.
typedef int (*SegmentProviderFn)(int32_t nodeLatE7, int32_t nodeLonE7, RoadSegment *out, int maxOut, void *ctx);

class Route {
public:
    static const int kMaxSegments = 64;     // hard cap (~2km at the ~31m median segment, far past any warn distance)
    static const int kMaxNodeCandidates = 12; // most a junction offers here (real data max node degree was 10)

    Route() { reset(); }

    // Clears the route (no segments). Safe to call any time.
    void reset();

    // Builds the forward route starting from `startSeg` (the segment the car is
    // currently matched to), traveling in the direction closest to
    // `startHeadingDeg`. Walks ahead via `provider`, choosing at each node the
    // incident segment that continues straightest (smallest turn from the
    // incoming heading) and that isn't already on the route (no U-turns/loops).
    // Stops at `aheadM` of accumulated length or kMaxSegments. Returns the
    // number of segments built (0 if even the start segment couldn't be placed).
    int build(const RoadSegment &startSeg, float startHeadingDeg, float aheadM, SegmentProviderFn provider, void *ctx);

    // Projects (latDeg,lonDeg) onto the route polyline. Returns true if the
    // nearest point is within `lateralTolM` perpendicular of the centerline AND
    // not behind the route origin. Outputs (any may be NULL): arc length from
    // the route origin, the snapped lat/lon, and the route's forward heading at
    // that point (the local travel direction — what a sign's own orientation
    // tag should be compared against, not the car's raw heading, around curves).
    bool project(float latDeg, float lonDeg, float lateralTolM, float *outDistM, float *outLat, float *outLon,
                 float *outHeadingDeg = 0, float *outLateralM = 0) const;

    // Arc length (from the route origin) of the first FORK the route passes:
    // a node where two or more branches both continue within kForkTurnDeg of
    // the incoming heading (Y-split, exit ramp vs mainline). Beyond it the
    // predicted path is a guess. Returns a huge value when there is none.
    float forkAtM() const { return forkAtM_; }
    static constexpr float kForkTurnDeg = 40.0f;

    bool containsSegment(uint32_t id) const;

    // Does this point (a sign / camera) belong to THIS route? True when it
    // projects onto the route within maxLateralM AND no OTHER road segment is
    // clearly closer (by more than marginM). nearIds/nearDistM: the road
    // segments around the point with their distances to it (caller's tile
    // data). Rejects a camera on the parallel service road, the opposite
    // carriageway of a divided road, or the road below a flyover.
    bool ownsPoint(float latDeg, float lonDeg, float maxLateralM, const uint32_t *nearIds, const float *nearDistM,
                   int nNear, float marginM, float *outDistM, float *outHeadingDeg) const;

    // Walks the route forward from arc length `fromDistM` and reports the first
    // segment whose speed limit differs from `currentLimitKmh`, with its
    // remaining distance from `fromDistM` (clamped >= 0). Returns false if the
    // limit never changes within `maxM` ahead. Replaces the straight-line
    // ahead-limit sampling: follows the actual road through curves/junctions.
    // taggedOnly: consider only segments whose limit comes from a real tag
    // (not SPEED_SOURCE_DEFAULT, a road-class guess) — a change between two
    // guesses is no evidence of a real sign.
    bool limitChangeAhead(float fromDistM, float currentLimitKmh, float maxM, float &outDistM,
                          float &outLimitKmh, bool taggedOnly = false) const;

    // The speed limit at arc length `distM` along the route. If `distM` is past
    // the built end, returns the last known limit on the route. Returns false
    // only if the route carries no known limit at all. Used to recover the
    // CURRENT limit when the live per-fix segment match wobbles or the matched
    // segment is untagged.
    bool limitAt(float distM, float &outLimitKmh) const;

    // Total arc length of the built route (0 if empty).
    float totalLenM() const;

    int count() const { return count_; }
    const RouteSeg &seg(int i) const { return segs_[i]; }

private:
    RouteSeg segs_[kMaxSegments];
    int count_;
    float originLat_, originLon_; // route polyline start (start node of segs_[0])
    float forkAtM_;               // see forkAtM()
};

// Distance (m) from a point to a road segment's centerline.
float segPointDistM(const RoadSegment &seg, float latDeg, float lonDeg);
