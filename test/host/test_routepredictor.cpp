// Host unit test for map/RoutePredictor.{h,cpp} — compiled with desktop clang,
// NO ESP32/Arduino/SD dependencies (that's the whole point of extracting Route
// into a standalone module). Drives Route::build/project/limitChangeAhead/
// limitAt with synthetic road networks covering the required scenarios.
//
// Build+run (from project root):
//   "/c/Program Files/LLVM/bin/clang++" -std=c++17 -I src/map \
//       .tmpwork/test_routepredictor.cpp src/map/RoutePredictor.cpp -o .tmpwork/trtest && .tmpwork/trtest
//
// SpeedMapFormat.h pulls in only <stdint.h>, so it compiles clean on the host.

#include "RoutePredictor.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- tiny test framework ----
static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    printf("  FAIL: %s  (line %d)\n", msg, __LINE__); } } while(0)
#define CHECK_NEAR(a, b, tol, msg) do { float _d = fabsf((float)(a)-(float)(b)); \
    if (_d <= (tol)) { g_pass++; } else { g_fail++; \
    printf("  FAIL: %s  got %.2f expected %.2f (tol %.2f, line %d)\n", msg, (double)(a),(double)(b),(double)(tol), __LINE__); } } while(0)

// ---- synthetic world: a flat list of RoadSegments the provider serves ----
// All test geometry is built near a base lat/lon so meters<->degrees is stable.
static const double BLAT = 21.0000, BLON = 105.8000;
static const double MPER_LAT = 110540.0;
static double mperLon() { return 111320.0 * cos(BLAT * M_PI / 180.0); }

static int32_t latE7(double northM) { return (int32_t)llround((BLAT + northM / MPER_LAT) * 1e7); }
static int32_t lonE7(double eastM)  { return (int32_t)llround((BLON + eastM / mperLon()) * 1e7); }
static double toNorthM(int32_t laE7) { return (laE7 / 1e7 - BLAT) * MPER_LAT; }
static double toEastM(int32_t loE7)  { return (loE7 / 1e7 - BLON) * mperLon(); }

// world segments the provider scans
static std::vector<RoadSegment> g_world;

static uint16_t headingOf(int32_t s_la, int32_t s_lo, int32_t e_la, int32_t e_lo) {
    double dn = toNorthM(e_la) - toNorthM(s_la);
    double de = toEastM(e_lo) - toEastM(s_lo);
    double deg = atan2(de, dn) * 180.0 / M_PI; // 0=N, clockwise
    if (deg < 0) deg += 360.0;
    return (uint16_t)llround(deg) % 360;
}

// Add a segment by endpoints in meters (east,north), returns its id.
static uint32_t addSeg(uint32_t id, double e0, double n0, double e1, double n1, int16_t limit,
                       uint8_t direction = DIR_BIDIRECTIONAL, uint8_t flags = 0) {
    RoadSegment s{};
    s.id = id;
    s.startLatE7 = latE7(n0); s.startLonE7 = lonE7(e0);
    s.endLatE7 = latE7(n1);   s.endLonE7 = lonE7(e1);
    s.headingDeg = headingOf(s.startLatE7, s.startLonE7, s.endLatE7, s.endLonE7);
    s.roadClass = 0;
    s.direction = direction;
    s.speedLimitKmh = limit;
    s.speedSource = 1;
    s.flags = flags;
    g_world.push_back(s);
    return id;
}

// Provider: return every world segment incident to (nodeLatE7,nodeLonE7).
static int worldProvider(int32_t nLa, int32_t nLo, RoadSegment *out, int maxOut, void *ctx) {
    (void)ctx;
    int n = 0;
    for (const auto &s : g_world) {
        if (n >= maxOut) break;
        bool inc = (s.startLatE7 == nLa && s.startLonE7 == nLo) ||
                   (s.endLatE7 == nLa && s.endLonE7 == nLo);
        if (inc) out[n++] = s;
    }
    return n;
}

static const RoadSegment &worldById(uint32_t id) {
    for (auto &s : g_world) if (s.id == id) return s;
    static RoadSegment dummy{}; return dummy;
}

// Convert an (east,north) meters point to lat/lon degrees for project() calls.
static void ptDeg(double eastM, double northM, float &lat, float &lon) {
    lat = (float)(BLAT + northM / MPER_LAT);
    lon = (float)(BLON + eastM / mperLon());
}

// ===================================================================
static void test_straight_road() {
    printf("[1] straight road: chain + arc length + limit\n");
    g_world.clear();
    // 5 segments east along y=0, 100m each, id 1..5, limit 60.
    for (int i = 0; i < 5; i++) addSeg(1 + i, i * 100.0, 0, (i + 1) * 100.0, 0, 60);
    Route r;
    int n = r.build(worldById(1), /*heading east*/ 90.0f, 600.0f, worldProvider, nullptr);
    CHECK(n == 5, "built all 5 straight segments");
    CHECK_NEAR(r.totalLenM(), 500.0f, 2.0f, "total length ~500m");
    // project a point at east=250,north=0 -> arc ~250
    float lat, lon, dist, snapLat, snapLon;
    ptDeg(250, 0, lat, lon);
    CHECK(r.project(lat, lon, 35.0f, &dist, &snapLat, &snapLon), "point on road projects");
    CHECK_NEAR(dist, 250.0f, 3.0f, "arc length ~250m");
    float lim;
    CHECK(r.limitAt(250.0f, lim) && lim == 60.0f, "limit at 250m == 60");
}

static void test_curve() {
    printf("[2] curve: straight-line would cut the corner, route follows it\n");
    g_world.clear();
    // An L-shaped road: 200m east, then 200m north. A straight projection from
    // the origin heading east would miss the northward leg entirely.
    addSeg(1, 0, 0, 200, 0, 50);
    addSeg(2, 200, 0, 200, 200, 50);
    Route r;
    int n = r.build(worldById(1), 90.0f, 600.0f, worldProvider, nullptr);
    CHECK(n == 2, "curve built both legs");
    CHECK_NEAR(r.totalLenM(), 400.0f, 3.0f, "L-route length ~400m");
    // A point on the northward leg (east=200, north=150) is ~350m along route.
    float lat, lon, dist;
    ptDeg(200, 150, lat, lon);
    CHECK(r.project(lat, lon, 35.0f, &dist, nullptr, nullptr), "point on 2nd leg projects onto route");
    CHECK_NEAR(dist, 350.0f, 5.0f, "arc length around the bend ~350m");
}

static void test_junction_straightest() {
    printf("[3] junction: picks the straightest continuation, not a turn\n");
    g_world.clear();
    // Main road east: seg1 (0->200), then at node(200,0) it splits:
    //   seg2 continues straight east (200->400)
    //   seg3 turns sharply north (200,0 -> 200,200)
    // Route must follow seg2.
    addSeg(1, 0, 0, 200, 0, 80);
    addSeg(2, 200, 0, 400, 0, 80);
    addSeg(3, 200, 0, 200, 200, 40);
    Route r;
    int n = r.build(worldById(1), 90.0f, 600.0f, worldProvider, nullptr);
    CHECK(n == 2, "junction route took 2 straight segments");
    CHECK(n >= 2 && r.seg(1).id == 2, "picked straight continuation (seg2), not the turn (seg3)");
}

static void test_limit_change_ahead() {
    printf("[4] speed-limit change ahead along the route\n");
    g_world.clear();
    // 60 for 300m, then 40. Change boundary at arc=300.
    addSeg(1, 0, 0, 100, 0, 60);
    addSeg(2, 100, 0, 200, 0, 60);
    addSeg(3, 200, 0, 300, 0, 60);
    addSeg(4, 300, 0, 400, 0, 40);
    addSeg(5, 400, 0, 500, 0, 40);
    Route r;
    r.build(worldById(1), 90.0f, 600.0f, worldProvider, nullptr);
    float d, lim;
    // From arc=50 (on seg1), change to 40 is at arc=300 -> 250m ahead.
    CHECK(r.limitChangeAhead(50.0f, 60.0f, 400.0f, d, lim), "found a limit change ahead");
    CHECK_NEAR(d, 250.0f, 1.0f, "change distance ~250m");
    CHECK(lim == 40.0f, "new limit == 40");
    // No change within a short horizon.
    CHECK(!r.limitChangeAhead(50.0f, 60.0f, 100.0f, d, lim), "no change within 100m horizon");
}

static void test_parallel_road_rejected() {
    printf("[5] sign/camera on a PARALLEL road is rejected by lateral offset\n");
    g_world.clear();
    // Our road along north=0. A parallel road 60m north (well beyond the 35m
    // tolerance). A camera sitting on the parallel road must NOT project onto us.
    for (int i = 0; i < 5; i++) addSeg(1 + i, i * 100.0, 0, (i + 1) * 100.0, 0, 50);
    Route r;
    r.build(worldById(1), 90.0f, 600.0f, worldProvider, nullptr);
    float lat, lon, dist;
    ptDeg(250, 60, lat, lon); // 60m north of our road
    CHECK(!r.project(lat, lon, 35.0f, &dist, nullptr, nullptr), "parallel-road point (60m off) rejected");
    // A point genuinely on our road (2m jitter) is accepted.
    ptDeg(250, 2, lat, lon);
    CHECK(r.project(lat, lon, 35.0f, &dist, nullptr, nullptr), "on-road point (2m off) accepted");
}

static void test_crossing_road_rejected() {
    printf("[6] sign on a CROSSING road, past the intersection, off our route\n");
    g_world.clear();
    // Our road east along north=0 for 400m. A crossing road runs north-south
    // through the intersection at east=200. A sign on the crossing road at
    // (200, 80) is 80m off our route laterally -> rejected.
    for (int i = 0; i < 4; i++) addSeg(1 + i, i * 100.0, 0, (i + 1) * 100.0, 0, 50);
    Route r;
    r.build(worldById(1), 90.0f, 600.0f, worldProvider, nullptr);
    float lat, lon, dist;
    ptDeg(200, 80, lat, lon);
    CHECK(!r.project(lat, lon, 35.0f, &dist, nullptr, nullptr), "crossing-road sign (80m off) rejected");
}

static void test_nearest_alert_selection() {
    printf("[7] nearest-along-route wins across mixed alert types\n");
    g_world.clear();
    for (int i = 0; i < 6; i++) addSeg(1 + i, i * 100.0, 0, (i + 1) * 100.0, 0, 50);
    Route r;
    r.build(worldById(1), 90.0f, 700.0f, worldProvider, nullptr);
    // Car at arc=50. A "camera" at east=350 (arc 350 -> 300m ahead) and a
    // "resident sign" at east=100 (arc 100 -> 50m ahead). Nearest is the sign.
    float carArc = 50.0f;
    float camLat, camLon, camArc, signLat, signLon, signArc;
    ptDeg(350, 1, camLat, camLon);
    ptDeg(100, 1, signLat, signLon);
    CHECK(r.project(camLat, camLon, 35.0f, &camArc, nullptr, nullptr), "camera projects");
    CHECK(r.project(signLat, signLon, 35.0f, &signArc, nullptr, nullptr), "sign projects");
    float camAhead = camArc - carArc, signAhead = signArc - carArc;
    CHECK_NEAR(camAhead, 300.0f, 3.0f, "camera 300m ahead");
    CHECK_NEAR(signAhead, 50.0f, 3.0f, "sign 50m ahead");
    CHECK(signAhead < camAhead, "sign is the nearest alert (would win the card)");
}

static void test_gps_loss_recovery() {
    printf("[8/9/11] GPS loss: route persists, re-snap recovers limit; rebuild on mismatch\n");
    g_world.clear();
    // Road A east, limit 70. Route built while on it.
    for (int i = 0; i < 6; i++) addSeg(1 + i, i * 100.0, 0, (i + 1) * 100.0, 0, 70);
    Route rA;
    rA.build(worldById(1), 90.0f, 700.0f, worldProvider, nullptr);
    // ---- [9] GPS returns on an UNTAGGED-in-this-fix spot: route still knows 70.
    float lim;
    CHECK(rA.limitAt(320.0f, lim) && lim == 70.0f, "limit recovered from route at arc=320 (no sign needed)");
    // ---- [8] re-snap a fresh fix onto the persisted route.
    float lat, lon, dist;
    ptDeg(320, 3, lat, lon);
    CHECK(rA.project(lat, lon, 35.0f, &dist, nullptr, nullptr), "fresh fix re-snaps onto persisted route");
    CHECK_NEAR(dist, 320.0f, 4.0f, "re-snapped arc ~320m");
    // ---- [11] a fix that no longer fits the old route (200m away) -> caller rebuilds.
    ptDeg(320, 200, lat, lon);
    CHECK(!rA.project(lat, lon, 35.0f, &dist, nullptr, nullptr), "far-off fix does NOT fit old route (triggers rebuild)");
}

static void test_untagged_segment_route_fill() {
    printf("[10] GPS returns on a segment with unknown limit -> route fills from neighbors\n");
    g_world.clear();
    // 60, then an UNKNOWN (-1) stretch, then 60 again. limitAt on the unknown
    // segment should still yield a value (falls back to a known one on route).
    addSeg(1, 0, 0, 100, 0, 60);
    addSeg(2, 100, 0, 200, 0, -1); // untagged
    addSeg(3, 200, 0, 300, 0, 60);
    Route r;
    r.build(worldById(1), 90.0f, 400.0f, worldProvider, nullptr);
    float lim;
    // On the untagged segment (arc ~150): limitAt returns the nearest known
    // going backward (60). This mirrors the firmware recovering a limit rather
    // than blanking on an untagged stretch.
    bool ok = r.limitAt(150.0f, lim);
    CHECK(ok, "limitAt returns a value on an untagged stretch");
    CHECK(lim == 60.0f, "untagged stretch inherits 60 from route");
}

static void test_oneway_wrongway() {
    printf("[extra] one-way orientation is honored in chaining\n");
    g_world.clear();
    // A one-way DIR_FORWARD segment (start->end only). Build heading east must
    // orient it forward; the route's first seg keeps start=origin.
    uint32_t id = addSeg(1, 0, 0, 150, 0, 50, DIR_FORWARD, SEGFLAG_ONEWAY);
    addSeg(2, 150, 0, 300, 0, 50, DIR_FORWARD, SEGFLAG_ONEWAY);
    (void)id;
    Route r;
    int n = r.build(worldById(1), 90.0f, 400.0f, worldProvider, nullptr);
    CHECK(n == 2, "one-way road chained forward");
    CHECK(r.seg(0).headingDeg > 45.0f && r.seg(0).headingDeg < 135.0f, "first seg heads east");
}

int main() {
    test_straight_road();
    test_curve();
    test_junction_straightest();
    test_limit_change_ahead();
    test_parallel_road_rejected();
    test_crossing_road_rejected();
    test_nearest_alert_selection();
    test_gps_loss_recovery();
    test_untagged_segment_route_fill();
    test_oneway_wrongway();
    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
