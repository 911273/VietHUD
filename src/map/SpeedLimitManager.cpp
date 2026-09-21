#include "SpeedLimitManager.h"
#include "SdCardManager.h"
#include "core/AppConfig.h" // cfg.demoMode — see kMaxMatchDistanceM's own comment
#include "core/SharedState.h"
#include "gnss/GNSS.h" // kGnssMotionThresholdKmh — ahead-lookahead only runs while actually moving
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

// ---------------------------------------------------------------------
// Tunable matching constants — concrete defaults for the spec's qualitative
// rules (sections 13-17), documented in the plan
// (C:\Users\phamq\.claude\plans\wobbly-swinging-chipmunk.md). Not yet
// exposed in Settings/AppConfig — same "ship a reasoned default, make it
// tunable once real data exists to tune against" approach this project used
// for the LD2451's own calibration before Settings > Radar existed.
// ---------------------------------------------------------------------
// Real driving value. cfg.demoMode (Settings > Radar) widens this at
// runtime instead — see kDemoMaxMatchDistanceM below — so a real drive
// never silently inherits a demo-only radius from a forgotten override.
static const float kMaxMatchDistanceM = 30.0f;  // candidates farther than this from the fix aren't considered at all
// Demo-mode-only radius (user-requested 2026-09-16, "them 1 nut bat tat thu
// nghiem trong menu", building on the earlier one-off "hien thi demo cac
// chuc nang de toi co the chinh sua truoc khi ra ngoai duong" ask): wide
// enough that an indoor GNSS fix confirmed ~105m from the nearest mapped
// road (Hoang Quoc Viet, id=21651, 60km/h) still clears kMinConfidence for
// previewing the speed-limit sign — distance-only confidence is
// 1-dist/radius (capped 0.6 with no heading), so 250 gives ~0.58 there vs.
// kMinConfidence=0.5. Only used while cfg.demoMode is on; real driving
// always uses kMaxMatchDistanceM above regardless of this value.
static const float kDemoMaxMatchDistanceM = 250.0f;
static const float kMinConfidence = 0.5f;       // below this, a match isn't trusted enough to switch to or report
static const float kSwitchMargin = 0.15f;       // a competing road must beat the current one by this much (after the continuity bonus) to steal the match
static const float kContinuityBonus = 0.1f;     // score bonus for staying on the road we were already matched to
static const uint32_t kMatchTimeoutMs = 8000;   // how long a low/no-confidence tick can hold the last good value before falling back to UNKNOWN

// 1024, not the earlier 512: a real Northern-Vietnam extract (tools/
// map_builder/, classified roads only, 2026-09-16) has a busiest single
// 0.01-degree tile with 752 segments — denser than the Hanoi-core test's
// 489-segment worst case that justified 512 in the first place. 512 would
// have silently TRUNCATED that tile's data (SdCardManager.cpp's readTile()
// caps at the buffer size it's given, with no error) rather than failing
// loudly, which is worse than just sizing for real data from the start.
static const int kMaxSegmentsPerTile = 1024;
static const int kCacheSize = 5; // current + N/S/E/W, per spec section 6

// At 512 segments x 28 bytes x 5 cached tiles = 70KB, this is PSRAM-backed
// (heap_caps_malloc, MALLOC_CAP_SPIRAM), not a plain static array in
// internal RAM — same reasoning main_ui_demo.cpp's LVGL draw buffers
// deliberately do the OPPOSITE (forced into internal RAM, PSRAM too slow
// for a per-frame render path), inverted: this cache is only touched once
// every ~500ms (see speedLimitTaskFn()'s loop), so PSRAM's higher latency
// is irrelevant, and freeing 70KB of the much scarcer internal RAM matters
// more here — especially once WiFi (net/WebPortal.cpp) is toggled on,
// which alone costs tens of KB of internal RAM.
struct CachedTile {
    bool valid = false;
    uint32_t tileId = 0;
    uint32_t lastUsedMs = 0;
    int segCount = 0;
    RoadSegment *segments = NULL; // PSRAM buffer, allocated once in speedLimitManagerStart()
};
static CachedTile cache[kCacheSize];

static void allocateTileCache() {
    for (int i = 0; i < kCacheSize; i++) {
        cache[i].segments =
            (RoadSegment *)heap_caps_malloc(sizeof(RoadSegment) * kMaxSegmentsPerTile, MALLOC_CAP_SPIRAM);
        if (!cache[i].segments) {
            // PSRAM exhausted or unavailable — extremely unlikely on this
            // board (8MB PSRAM, barely used elsewhere) but fall back to
            // internal RAM rather than leave a null pointer that would
            // crash the first time this slot is used.
            Serial.println("[map] WARN: PSRAM allocation for tile cache failed, falling back to internal RAM");
            cache[i].segments = (RoadSegment *)malloc(sizeof(RoadSegment) * kMaxSegmentsPerTile);
        }
    }
}

static SpeedMapMetadata gMetadata;
static bool gMapLoaded = false;
static uint32_t currentRoadId = 0;
static float currentConfidence = 0;
static uint32_t lastValidMs = 0;
static RoadInfoSnapshot lastPublished;

bool speedLimitManagerGetInfo(SpeedMapMetadata &out) {
    if (!gMapLoaded) return false;
    out = gMetadata;
    return true;
}

const char *speedSourceStr(uint8_t source) {
    switch (source) {
        case SPEED_SOURCE_OSM_MAXSPEED: return "OSM_MAXSPEED";
        case SPEED_SOURCE_OSM_FORWARD: return "OSM_FORWARD";
        case SPEED_SOURCE_OSM_BACKWARD: return "OSM_BACKWARD";
        case SPEED_SOURCE_DEFAULT: return "DEFAULT";
        case SPEED_SOURCE_CONDITIONAL: return "CONDITIONAL";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------
// Geometry — local flat-earth projection around each segment's own start
// point. Good to a few cm of error over the ~30m match radius this is used
// at (equirectangular approximation, not a proper geodesic), which is far
// tighter than the GNSS fix's own accuracy — no need for anything more
// precise here.
// ---------------------------------------------------------------------
struct Vec2 {
    float x, y;
};

static Vec2 toLocalMeters(float latDeg, float lonDeg, float originLatDeg, float originLonDeg) {
    float dLat = latDeg - originLatDeg;
    float dLon = lonDeg - originLonDeg;
    float y = dLat * 110540.0f;
    float x = dLon * 111320.0f * cosf(originLatDeg * (float)M_PI / 180.0f);
    return {x, y};
}

static float pointSegmentDistanceM(float fixLat, float fixLon, float startLat, float startLon, float endLat,
                                    float endLon) {
    Vec2 p = toLocalMeters(fixLat, fixLon, startLat, startLon);
    Vec2 e = toLocalMeters(endLat, endLon, startLat, startLon);
    float segLenSq = e.x * e.x + e.y * e.y;
    float t = segLenSq > 1e-6f ? (p.x * e.x + p.y * e.y) / segLenSq : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float dx = p.x - e.x * t, dy = p.y - e.y * t;
    return sqrtf(dx * dx + dy * dy);
}

static float angularDiffDeg(float a, float b) {
    float d = fmodf(fabsf(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

// ---------------------------------------------------------------------
// Candidate scoring — see the plan's "Map matching algorithm" section for
// the reasoning behind each rule (direction disqualification vs. heading
// penalty, the heading-unavailable confidence cap, etc).
// ---------------------------------------------------------------------
struct MatchCandidate {
    uint32_t roadId;
    float distanceM;
    float confidence;
    int16_t speedLimitKmh;
    uint8_t source;
};

static bool evaluateSegment(const RoadSegment &seg, float fixLat, float fixLon, bool headingValid, float headingDeg,
                             MatchCandidate &outCand) {
    // See kDemoMaxMatchDistanceM's own comment — demo mode trades match
    // precision for being able to preview the sign away from a real road.
    float maxMatchDistanceM = cfg.demoMode ? kDemoMaxMatchDistanceM : kMaxMatchDistanceM;

    float startLat = seg.startLatE7 / 1e7f, startLon = seg.startLonE7 / 1e7f;
    float endLat = seg.endLatE7 / 1e7f, endLon = seg.endLonE7 / 1e7f;
    float dist = pointSegmentDistanceM(fixLat, fixLon, startLat, startLon, endLat, endLon);
    if (dist > maxMatchDistanceM) return false;

    float forwardHeading = (float)seg.headingDeg;
    float backwardHeading = fmodf(forwardHeading + 180.0f, 360.0f);

    float headingErr = 0;
    if (headingValid) {
        if (seg.direction == DIR_FORWARD) headingErr = angularDiffDeg(headingDeg, forwardHeading);
        else if (seg.direction == DIR_BACKWARD) headingErr = angularDiffDeg(headingDeg, backwardHeading);
        else { // BIDIRECTIONAL/UNKNOWN: whichever of the two directions of travel this segment allows is closer
            float e1 = angularDiffDeg(headingDeg, forwardHeading);
            float e2 = angularDiffDeg(headingDeg, backwardHeading);
            headingErr = e1 < e2 ? e1 : e2;
        }
    }

    // A genuine legal one-way (not just "this record only carries one
    // direction's speed value" — see SEGFLAG_ONEWAY's own comment) being
    // approached the wrong way is disqualified outright, never merely
    // penalized (spec 9/13.3). Only enforced when heading is trustworthy —
    // an unreliable heading must exclude a candidate from this check
    // entirely rather than risk silently waving through a wrong-way match.
    if ((seg.flags & SEGFLAG_ONEWAY) && headingValid && headingErr > 90.0f) return false;

    float confidence;
    if (headingValid) {
        confidence = 1.0f - dist / maxMatchDistanceM - headingErr / 90.0f;
    } else {
        // No heading corroboration available (GNSS.cpp only reports one
        // once moving above its own minimum-speed gate) — distance-only
        // score, capped below kMinConfidence's usual "clearly a good match"
        // territory so a low-speed/stationary reading can't alone promote
        // a road to HIGH confidence.
        confidence = 1.0f - dist / maxMatchDistanceM;
        if (confidence > 0.6f) confidence = 0.6f;
    }
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;

    outCand.roadId = seg.id;
    outCand.distanceM = dist;
    outCand.confidence = confidence;
    outCand.speedLimitKmh = seg.speedLimitKmh;
    outCand.source = seg.speedSource;
    return true;
}

// ---------------------------------------------------------------------
// Tile cache — see spec sections 20-21: never read the whole database into
// RAM, only the current tile + its 4 neighbors, evicting least-recently-used
// when the small fixed cache is full.
// ---------------------------------------------------------------------
static uint32_t packTile(int32_t latCell, int32_t lonCell) {
    return ((uint32_t)latCell << 16) | ((uint32_t)lonCell & 0xFFFF);
}

static CachedTile *findCachedTile(uint32_t tileId) {
    for (int i = 0; i < kCacheSize; i++)
        if (cache[i].valid && cache[i].tileId == tileId) return &cache[i];
    return NULL;
}

static const CachedTile *getOrLoadTile(uint32_t tileId) {
    CachedTile *hit = findCachedTile(tileId);
    if (hit) {
        hit->lastUsedMs = millis();
        return hit;
    }

    // Only touch SD for a tile that actually exists in the index — most of
    // the 5 candidates around any given fix legitimately don't exist yet in
    // the database, and that's not an error worth a wasted file-open
    // attempt.
    //
    // Binary search, not a linear scan (switched 2026-09-16 alongside a
    // real Northern-Vietnam extract reaching 62081 index entries — a linear
    // scan of that many PSRAM reads, 5 times per match tick, was real
    // overhead a small Hanoi-core test's 165 entries never surfaced).
    // SpeedMapFormat.h's own TileIndexEntry comment documents the index as
    // sorted by tileId ascending specifically so this is valid — confirmed
    // against build_speedmap.py's build_database(), which writes entries via
    // `for tid, segs in sorted(tiles.items())`.
    const TileIndexEntry *index;
    int indexCount;
    if (!sdMgrGetIndex(&index, &indexCount)) return NULL;
    int foundIdx = -1;
    int lo = 0, hi = indexCount - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (index[mid].tileId == tileId) {
            foundIdx = mid;
            break;
        } else if (index[mid].tileId < tileId) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (foundIdx < 0) return NULL;

    // Guard the extremely-unlikely case where allocateTileCache() couldn't
    // get memory for this slot at all (PSRAM AND internal RAM both
    // exhausted) — treat it the same as "tile not cached", rather than
    // writing through a null pointer.
    int victim = 0;
    for (int i = 1; i < kCacheSize; i++) {
        if (!cache[i].valid) {
            victim = i;
            break;
        }
        if (cache[i].lastUsedMs < cache[victim].lastUsedMs) victim = i;
    }
    if (!cache[victim].segments) return NULL;
    int n = 0;
    sdMgrReadTile(index[foundIdx], cache[victim].segments, kMaxSegmentsPerTile, &n);
    cache[victim].valid = true;
    cache[victim].tileId = tileId;
    cache[victim].segCount = n;
    cache[victim].lastUsedMs = millis();
    return &cache[victim];
}

// ---------------------------------------------------------------------
// Top-level match — reads lastPublished (previous tick's result) for the
// hold-on-low-confidence behavior but never writes it; the caller (the task
// loop, or runSelfTest()) owns updating lastPublished after each call. See
// SpeedLimitManager.h's file comment and the plan for the overall flow.
// ---------------------------------------------------------------------
static void runMatch(const GnssSnapshot &gnss, RoadInfoSnapshot &out) {
    out.mapLoaded = true;

    // Spec section 30: no fix means UNKNOWN immediately, no holding a
    // stale value — a fundamentally different situation from "still have a
    // fix but the map match briefly got noisy" (the timeout/hold logic
    // below), so this returns early rather than flowing into it.
    if (!gnss.fix) return;

    int32_t latCell = (int32_t)((gnss.latDeg + 90.0f) / gMetadata.tileSizeDeg);
    int32_t lonCell = (int32_t)((gnss.lonDeg + 180.0f) / gMetadata.tileSizeDeg);
    uint32_t tileIds[5] = {packTile(latCell, lonCell), packTile(latCell + 1, lonCell),
                            packTile(latCell - 1, lonCell), packTile(latCell, lonCell + 1),
                            packTile(latCell, lonCell - 1)};

    MatchCandidate best = {};
    bool haveBest = false;
    MatchCandidate bestForCurrentRoad = {};
    bool haveCurrentRoad = false;

    for (int i = 0; i < 5; i++) {
        const CachedTile *tile = getOrLoadTile(tileIds[i]);
        if (!tile) continue;
        for (int s = 0; s < tile->segCount; s++) {
            MatchCandidate cand;
            if (!evaluateSegment(tile->segments[s], gnss.latDeg, gnss.lonDeg, gnss.headingValid, gnss.headingDeg,
                                  cand))
                continue;
            if (cand.roadId == currentRoadId) {
                bestForCurrentRoad = cand;
                haveCurrentRoad = true;
            }
            if (!haveBest || cand.confidence > best.confidence) {
                best = cand;
                haveBest = true;
            }
        }
    }

    MatchCandidate chosen;
    bool haveChosen = false;
    if (haveBest) {
        // Hysteresis + continuity (spec section 15, test 3): stay on the
        // road we're already matched to unless a competitor clearly beats
        // it even after this road gets its continuity bonus.
        if (haveCurrentRoad && (bestForCurrentRoad.confidence + kContinuityBonus) >= (best.confidence - kSwitchMargin)) {
            chosen = bestForCurrentRoad;
        } else {
            chosen = best;
        }
        haveChosen = true;
    }

    if (!haveChosen || chosen.confidence < kMinConfidence) {
        // Nothing usable this tick — hold the previous valid result briefly
        // (spec section 17) rather than flapping to UNKNOWN on one noisy
        // sample, but only up to kMatchTimeoutMs (spec section 32).
        if (currentRoadId != 0 && millis() - lastValidMs < kMatchTimeoutMs) {
            out.valid = lastPublished.valid;
            out.speedLimitKmh = lastPublished.speedLimitKmh;
            out.source = lastPublished.source;
            out.confidence = lastPublished.confidence;
            out.roadId = currentRoadId;
            out.matchDistanceM = -1;
        }
        return;
    }

    currentRoadId = chosen.roadId;
    currentConfidence = chosen.confidence;
    lastValidMs = millis();

    out.roadId = chosen.roadId;
    out.confidence = chosen.confidence;
    out.matchDistanceM = chosen.distanceM;
    out.source = chosen.source;
    if (chosen.speedLimitKmh >= 0) {
        out.valid = true;
        out.speedLimitKmh = (float)chosen.speedLimitKmh;
    } else {
        // Matched a real road, just one with no known speed limit tag —
        // still an honest UNKNOWN for display, not a fabricated number.
        out.valid = false;
        out.speedLimitKmh = -1;
    }
}

// ---------------------------------------------------------------------
// Upcoming speed-limit-change lookahead (user-requested 2026-09-21, "bo
// sung chuc nang canh bao gioi han toc do doan duong tiep theo, bao truoc
// khoang 100m"). Projects a point kAheadLookaheadM ahead along the current
// heading (same flat-earth approximation as toLocalMeters above, inverted —
// fine at this short a range) and runs the SAME segment-matching scoring
// the live position match uses (evaluateSegment/getOrLoadTile), just
// anchored at that projected point instead of the real fix. Deliberately
// no hysteresis/continuity/hold-timeout the way runMatch()'s live tracking
// has: this is a one-shot snapshot query, recomputed fresh every tick, not
// a continuously-tracked state — a transient bad read here just means one
// tick without an ahead-warning, not a wrong CURRENT-road decision.
static const float kAheadLookaheadM = 100.0f;

static bool matchAheadPoint(const GnssSnapshot &gnss, MatchCandidate &outBest) {
    float rad = gnss.headingDeg * (float)M_PI / 180.0f;
    float dx = kAheadLookaheadM * sinf(rad);  // east component
    float dy = kAheadLookaheadM * cosf(rad);  // north component
    float aheadLat = gnss.latDeg + dy / 110540.0f;
    float aheadLon = gnss.lonDeg + dx / (111320.0f * cosf(gnss.latDeg * (float)M_PI / 180.0f));

    int32_t latCell = (int32_t)((aheadLat + 90.0f) / gMetadata.tileSizeDeg);
    int32_t lonCell = (int32_t)((aheadLon + 180.0f) / gMetadata.tileSizeDeg);
    uint32_t tileIds[5] = {packTile(latCell, lonCell), packTile(latCell + 1, lonCell),
                            packTile(latCell - 1, lonCell), packTile(latCell, lonCell + 1),
                            packTile(latCell, lonCell - 1)};

    bool haveBest = false;
    for (int i = 0; i < 5; i++) {
        const CachedTile *tile = getOrLoadTile(tileIds[i]);
        if (!tile) continue;
        for (int s = 0; s < tile->segCount; s++) {
            MatchCandidate cand;
            if (!evaluateSegment(tile->segments[s], aheadLat, aheadLon, gnss.headingValid, gnss.headingDeg, cand))
                continue;
            if (!haveBest || cand.confidence > outBest.confidence) {
                outBest = cand;
                haveBest = true;
            }
        }
    }
    return haveBest && outBest.confidence >= kMinConfidence && outBest.speedLimitKmh >= 0;
}

// ---------------------------------------------------------------------
// Upcoming speed-camera warning (feature-requested 2026-09-21, "tai du lieu
// ve canh bao giao thong, gom camera cung nhu gioi han toc do"). Scans the
// WHOLE flat CameraPoint[] (map/SdCardManager.cpp's sdMgrGetCameras() — no
// tiling needed, see that array's own comment on why real camera counts
// stay small) rather than doing a tile lookup the way the road matcher
// does — simplest correct approach for what's at most a few thousand
// points, and this only runs once per ~500ms match tick, not per frame.
// Completely independent of runMatch()'s own road-segment result/
// hysteresis state: a camera can be "ahead" whether or not the current
// road segment itself matched.
static const float kCameraWarnDistanceM = 300.0f; // start warning this far out — real turn-by-turn nav apps commonly warn 200-500m before a camera
static const float kCameraBearingToleranceDeg = 60.0f; // how far off dead-ahead a camera can be and still count as "ahead" rather than off to the side/behind

static void matchCameraAhead(const GnssSnapshot &gnss, const RoadInfoSnapshot &roadMatch, RoadInfoSnapshot &out) {
    if (!gnss.fix || !gnss.headingValid) return; // same "don't guess direction without a trustworthy heading" gate the ahead-limit lookahead above uses
    const CameraPoint *cams;
    int camCount;
    sdMgrGetCameras(&cams, &camCount);
    if (camCount == 0) return;

    float bestDistM = 1e9f;
    const CameraPoint *best = NULL;
    for (int i = 0; i < camCount; i++) {
        float camLat = cams[i].latE7 / 1e7f, camLon = cams[i].lonE7 / 1e7f;
        Vec2 p = toLocalMeters(camLat, camLon, gnss.latDeg, gnss.lonDeg); // camera's offset FROM the fix
        float distM = sqrtf(p.x * p.x + p.y * p.y);
        if (distM > kCameraWarnDistanceM || distM >= bestDistM) continue; // already farther than the current best — skip the trig below for it
        float bearingToCameraDeg = atan2f(p.x, p.y) * (180.0f / (float)M_PI); // 0=north/clockwise, same convention as headingDeg
        if (bearingToCameraDeg < 0) bearingToCameraDeg += 360.0f;
        if (angularDiffDeg(gnss.headingDeg, bearingToCameraDeg) > kCameraBearingToleranceDeg) continue;
        bestDistM = distM;
        best = &cams[i];
    }
    if (!best) return;

    out.cameraAheadValid = true;
    out.cameraAheadDistanceM = bestDistM;
    out.cameraSpeedLimitKmh = (best->speedLimitKmh >= 0) ? (float)best->speedLimitKmh
                               : (roadMatch.valid ? roadMatch.speedLimitKmh : -1.0f);
}

static const float kSignWarnDistanceM = 350.0f;
static const float kSignBearingToleranceDeg = 50.0f;

static void matchSignsAhead(const GnssSnapshot &gnss, RoadInfoSnapshot &out) {
    if (!gnss.fix || !gnss.headingValid) return;
    const TrafficSignPoint *signs;
    int signCount;
    sdMgrGetSigns(&signs, &signCount);
    if (signCount == 0) return;

    float closestSignDistM = 1e9f;
    const TrafficSignPoint *closestSign = NULL;

    float bestResidentDistM = 1e9f;
    const TrafficSignPoint *bestResident = NULL;

    float bestNoOvertakeDistM = 1e9f;
    const TrafficSignPoint *bestNoOvertake = NULL;

    float bestTollDistM = 1e9f;
    const TrafficSignPoint *bestToll = NULL;

    float bestLightDistM = 1e9f;
    const TrafficSignPoint *bestLight = NULL;

    for (int i = 0; i < signCount; i++) {
        float signLat = signs[i].latE7 / 1e7f;
        float signLon = signs[i].lonE7 / 1e7f;

        // Fast bounding box check (lat/lon)
        float dLat = fabsf(signLat - gnss.latDeg);
        if (dLat > 0.005f) continue; // ~550m

        Vec2 p = toLocalMeters(signLat, signLon, gnss.latDeg, gnss.lonDeg);
        float distM = sqrtf(p.x * p.x + p.y * p.y);
        if (distM > kSignWarnDistanceM) continue;

        // Check bearing to sign from car
        float bearingToSignDeg = atan2f(p.x, p.y) * (180.0f / (float)M_PI);
        if (bearingToSignDeg < 0) bearingToSignDeg += 360.0f;
        if (angularDiffDeg(gnss.headingDeg, bearingToSignDeg) > kSignBearingToleranceDeg) continue;

        // Also check sign's own orientation if tagged
        if (signs[i].directionDeg != 0xFFFF) {
            if (angularDiffDeg(gnss.headingDeg, signs[i].directionDeg) > 60.0f) {
                continue; // Sign faces away or perpendicular to car's travel lane
            }
        }

        if (distM < closestSignDistM) {
            closestSignDistM = distM;
            closestSign = &signs[i];
        }

        // Category sorting
        if (signs[i].signType == SIGN_TYPE_RESIDENT_AREA && distM < bestResidentDistM) {
            bestResidentDistM = distM;
            bestResident = &signs[i];
        } else if (signs[i].signType == SIGN_TYPE_NO_OVERTAKING && distM < bestNoOvertakeDistM) {
            bestNoOvertakeDistM = distM;
            bestNoOvertake = &signs[i];
        } else if (signs[i].signType == SIGN_TYPE_TOLL_BOOTH && distM < bestTollDistM) {
            bestTollDistM = distM;
            bestToll = &signs[i];
        } else if (signs[i].signType == SIGN_TYPE_TRAFFIC_LIGHT && distM < bestLightDistM) {
            bestLightDistM = distM;
            bestLight = &signs[i];
        }
    }

    if (bestResident) {
        out.residentAreaAheadValid = true;
        out.residentAreaAheadDistM = bestResidentDistM;
        out.residentAreaIsStart = (bestResident->subType == 0);
    }
    if (bestNoOvertake) {
        out.noOvertakingAheadValid = true;
        out.noOvertakingAheadDistM = bestNoOvertakeDistM;
        out.noOvertakingIsStart = (bestNoOvertake->subType == 0);
    }
    if (bestToll) {
        out.tollBoothAheadValid = true;
        out.tollBoothAheadDistM = bestTollDistM;
    }
    if (bestLight) {
        out.trafficLightAheadValid = true;
        out.trafficLightAheadDistM = bestLightDistM;
    }
    if (closestSign) {
        out.nextSignType = closestSign->signType;
        out.nextSignDistanceM = closestSignDistM;
        out.nextSignSpeedLimit = closestSign->speedLimitKmh;
    }
}

// ---------------------------------------------------------------------
static void runSelfTest() {
    if (gMetadata.testRoadId == 0) {
        Serial.println("[map] database has no self-test point (testRoadId=0) — skipping self-test");
        return;
    }
    GnssSnapshot fake;
    fake.fix = true;
    fake.latDeg = gMetadata.testLatE7 / 1e7f;
    fake.lonDeg = gMetadata.testLonE7 / 1e7f;
    fake.headingValid = true;
    fake.headingDeg = (float)gMetadata.testHeadingDeg;

    RoadInfoSnapshot result;
    runMatch(fake, result);
    bool pass = result.valid && result.roadId == gMetadata.testRoadId;
    Serial.printf("[map] self-test %s — expected road=%lu got road=%lu limit=%.0fkm/h confidence=%.2f\n",
                  pass ? "PASS" : "FAIL", (unsigned long)gMetadata.testRoadId, (unsigned long)result.roadId,
                  (double)result.speedLimitKmh, (double)result.confidence);

    // Reset continuity state so the synthetic self-test point doesn't bias
    // the very first real match via the continuity bonus.
    currentRoadId = 0;
    currentConfidence = 0;
    lastValidMs = 0;
    lastPublished = RoadInfoSnapshot();
}

static void speedLimitTaskFn(void *) {
    allocateTileCache();
    RoadInfoSnapshot ri;
    if (!sdMgrMount()) {
        Serial.println("[map] SD card/database unavailable — speed limit will report UNKNOWN (mapLoaded=false)");
    } else {
        sdMgrGetMetadata(gMetadata);
        ri.mapLoaded = true;
        gMapLoaded = true;
        runSelfTest();
    }
    lastPublished = ri;
    roadInfoPublish(ri);

    esp_task_wdt_add(NULL);
    uint32_t lastDebugMs = 0;
    for (;;) {
        if (ri.mapLoaded) {
            GnssSnapshot gnss = gnssSnapshot();
            RoadInfoSnapshot out;
            runMatch(gnss, out);

            // Ahead-lookahead (see matchAheadPoint's own comment) — only
            // worth projecting forward while actually moving with a
            // trustworthy heading (a stopped/slow vehicle's heading is
            // noise, same gate GNSS.cpp itself already uses for
            // headingValid's own reliability) and only once we KNOW the
            // current limit (out.valid) so there's something real to
            // compare the ahead-match against. Only surfaced when it's a
            // genuinely DIFFERENT limit — re-confirming the same number
            // ahead isn't a "change" worth a driver's attention.
            if (gnss.headingValid && gnss.egoSpeedKmh > kGnssMotionThresholdKmh && out.valid) {
                MatchCandidate ahead;
                if (matchAheadPoint(gnss, ahead) && (float)ahead.speedLimitKmh != out.speedLimitKmh) {
                    out.aheadLimitValid = true;
                    out.aheadSpeedLimitKmh = (float)ahead.speedLimitKmh;
                    out.aheadDistanceM = kAheadLookaheadM;
                }
            }

            // Camera warning (see matchCameraAhead's own comment) — same
            // "only worth it while actually moving with a trustworthy
            // heading" gate as the ahead-limit lookahead above, but
            // deliberately NOT gated on out.valid: a camera is worth
            // knowing about even on a road segment whose own speed limit
            // this firmware doesn't know.
            if (gnss.headingValid && gnss.egoSpeedKmh > kGnssMotionThresholdKmh) {
                matchCameraAhead(gnss, out, out);
                matchSignsAhead(gnss, out);
            }

            lastPublished = out;
            roadInfoPublish(out);

            // Same 3s periodic-debug pattern as gnss/GNSS.cpp and
            // radar/LD2451.cpp — lat/lon isn't shown anywhere in the UI, so
            // this is the only way to see whether a real fix is actually
            // landing inside the downloaded map region at all (user-reported
            // 2026-09-16, "toa do GPS da len nhung khong hien thi toc do gioi
            // han" — need to see the real numbers to tell "outside the
            // downloaded region" apart from "in region but not matching"
            // apart from a genuine bug).
            uint32_t now = millis();
            if (now - lastDebugMs > 3000) {
                lastDebugMs = now;
                Serial.printf("[map] fix=%d lat=%.6f lon=%.6f valid=%d roadId=%lu conf=%.2f distM=%.1f "
                              "limit=%.0f source=%s\n",
                              gnss.fix, (double)gnss.latDeg, (double)gnss.lonDeg, out.valid,
                              (unsigned long)out.roadId, (double)out.confidence, (double)out.matchDistanceM,
                              (double)out.speedLimitKmh, speedSourceStr(out.source));
            }
        }
        esp_task_wdt_reset();
        // 500ms matches spec section 21 ("Map Matching: 1-5 Hz", "không cần
        // chạy map matching ở 30 FPS") while there's real work to do; 2s
        // idle once mapLoaded is permanently false (can't become true again
        // without a reboot), same idle-slower reasoning
        // net/WebPortal.cpp's webTaskFn uses for its own OFF-state delay.
        vTaskDelay(pdMS_TO_TICKS(ri.mapLoaded ? 500 : 2000));
    }
}

// Kept at 4096, NOT shrunk like radar/LD2451.cpp's and gnss/GNSS.cpp's own
// tasks got in the same 2026-09-16 RAM audit: this one's measured stack
// high-water mark was only ~920 bytes free (~22% margin, using ~3176 of
// 4096) — already the tightest of every task in this project, and it does
// real file I/O (SD_MMC File objects) unlike the others, so it's kept as
// the one exception rather than trimmed on the same formula.
void speedLimitManagerStart() { xTaskCreatePinnedToCore(speedLimitTaskFn, "speedLimitTask", 4096, NULL, 1, NULL, 0); }
