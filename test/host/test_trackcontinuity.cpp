// Host test for map/TrackContinuity — elevated road stacked over a surface road.
// Simulates a car on a small synthetic network with realistic GPS error and
// compares the OLD per-fix choice (continuity only for the very same segment)
// with the NEW topology-aware choice. Build: test/host/run_track.sh
#include "../../src/map/TrackContinuity.h"
#include "../../src/map/SpeedMapFormat.h"
#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Seg {
    uint32_t id;
    double x1, y1, x2, y2;
    char road; // 'G' surface, 'E' elevated, 'R' ramp, 'C' cross street
    uint8_t flags;
};
static std::vector<Seg> segs;
static std::vector<std::vector<int>> adj;

static int addSeg(char road, double x1, double y1, double x2, double y2, uint8_t flags = 0) {
    segs.push_back({(uint32_t)(segs.size() + 1), x1, y1, x2, y2, road, flags});
    return (int)segs.size() - 1;
}
static bool same(double a, double b) { return std::fabs(a - b) < 1e-6; }
static void buildAdjacency() {
    adj.assign(segs.size(), {});
    for (size_t i = 0; i < segs.size(); i++)
        for (size_t j = 0; j < segs.size(); j++) {
            if (i == j) continue;
            const Seg &a = segs[i], &b = segs[j];
            double ax[2] = {a.x1, a.x2}, ay[2] = {a.y1, a.y2}, bx[2] = {b.x1, b.x2}, by[2] = {b.y1, b.y2};
            bool touch = false;
            for (int p = 0; p < 2; p++)
                for (int q = 0; q < 2; q++)
                    if (same(ax[p], bx[q]) && same(ay[p], by[q]) && a.road != 'X') touch = true;
            if (touch) adj[i].push_back((int)j);
        }
}

static void buildNetwork(double offsetY) {
    segs.clear();
    // Surface road G: x 0..2100, 30 m segments, with cross streets every 90 m.
    for (double x = 0; x < 2100; x += 30) {
        addSeg('G', x, 0, x + 30, 0);
        if (std::fmod(x, 90.0) == 0 && x > 0) {
            addSeg('C', x, 0, x, 60);
            addSeg('C', x, 0, x, -60);
        }
    }
    // On-ramp: leaves G at x=150, climbs beside it, joins E at x=330.
    addSeg('R', 150, 0, 210, -12, SEGFLAG_LINK);
    addSeg('R', 210, -12, 270, -12, SEGFLAG_LINK);
    addSeg('R', 270, -12, 335, offsetY, SEGFLAG_LINK);
    // Elevated E: x 335..1505 (bridge), right above / beside G. Its nodes are its
    // own (x offset 5 m from G's) — a viaduct shares no nodes with the road below.
    for (double x = 335; x < 1505; x += 30) addSeg('E', x, offsetY, x + 30, offsetY, SEGFLAG_BRIDGE);
    // Off-ramp: E at x=1505 down to G at x=1680.
    addSeg('R', 1505, offsetY, 1560, -12, SEGFLAG_LINK);
    addSeg('R', 1560, -12, 1620, -12, SEGFLAG_LINK);
    addSeg('R', 1620, -12, 1680, 0, SEGFLAG_LINK);
    buildAdjacency();
}

static double distPointSeg(double px, double py, const Seg &s, double *headingOut) {
    double ex = s.x2 - s.x1, ey = s.y2 - s.y1;
    double L2 = ex * ex + ey * ey;
    double t = L2 > 0 ? ((px - s.x1) * ex + (py - s.y1) * ey) / L2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    double dx = px - (s.x1 + ex * t), dy = py - (s.y1 + ey * t);
    if (headingOut) *headingOut = std::atan2(ex, ey) * 180.0 / M_PI; // compass: 0=N(+y), 90=E(+x)
    return std::sqrt(dx * dx + dy * dy);
}

// Same geometry as SpeedLimitManager::evaluateSegment (bidirectional segments):
// distance, heading error, and the old confidence score (for the baseline).
static bool score(const Seg &s, double px, double py, double headingDeg, float &conf, float &dist, float &herr) {
    double h;
    double d = distPointSeg(px, py, s, &h);
    if (d > 55) return false;
    double e1 = std::fabs(std::fmod(std::fabs(headingDeg - h), 360.0));
    if (e1 > 180) e1 = 360 - e1;
    double e2 = 180 - e1;
    double err = e1 < e2 ? e1 : e2;
    double cosH = std::cos(err * M_PI / 180.0);
    if (cosH < 0) cosH = 0;
    conf = (float)((1.0 - d / (55 * 1.25)) * (0.30 + 0.70 * cosH));
    if (conf < 0) conf = 0;
    dist = (float)d;
    herr = (float)err;
    return true;
}

// Neighbour callback as the firmware provides it: segments sharing a node, up to two hops.
static int nbFn(uint32_t segId, uint32_t *out, int maxOut, void *) {
    int n = 0;
    int i0 = (int)segId - 1;
    std::vector<int> seen = {i0};
    std::vector<int> frontier = {i0};
    for (int depth = 0; depth < 2; depth++) {
        std::vector<int> next;
        for (int f : frontier)
            for (int nb : adj[f]) {
                bool s2 = false;
                for (int x : seen) if (x == nb) s2 = true;
                if (!s2 && n < maxOut) { seen.push_back(nb); next.push_back(nb); out[n++] = segs[nb].id; }
            }
        frontier = next;
    }
    return n;
}

struct Path { std::vector<std::pair<double, double>> pts; std::vector<char> road; };

// Truth trajectory sampled every 20 m (72 km/h at 1 Hz).
static Path makePath(bool takeElevated) {
    Path p;
    auto seg = [&](double x1, double y1, double x2, double y2, char r) {
        double L = std::hypot(x2 - x1, y2 - y1);
        for (double s = 0; s < L; s += 20) {
            p.pts.push_back({x1 + (x2 - x1) * s / L, y1 + (y2 - y1) * s / L});
            p.road.push_back(r);
        }
    };
    if (!takeElevated) {
        seg(0, 0, 2100, 0, 'G');
        return p;
    }
    double oy = segs[0].y1; (void)oy;
    double eY = 0;
    for (auto &s : segs) if (s.road == 'E') { eY = s.y1; break; }
    seg(0, 0, 150, 0, 'G');
    seg(150, 0, 210, -12, 'R'); seg(210, -12, 270, -12, 'R'); seg(270, -12, 335, eY, 'R');
    seg(335, eY, 1505, eY, 'E');
    seg(1505, eY, 1560, -12, 'R'); seg(1560, -12, 1620, -12, 'R'); seg(1620, -12, 1680, 0, 'R');
    seg(1680, 0, 2100, 0, 'G');
    return p;
}

// Environment: open sky vs. under the viaduct (surface road below E, where GPS
// multipath is bad). gradeFlags: data carries SEGFLAG_BRIDGE (rebuilt cards).
struct Env { double openNoise, openBias, underNoise, underBias; bool gradeFlags; bool altitude; };
static bool underViaduct(char road, double x) { return road == 'G' && x > 335 && x < 1505; }
static double trueAltitude(char road, double x) {
    if (road == 'E') return 10.0;
    if (road == 'R') {
        if (x < 1000) return x <= 150 ? 0.0 : (x >= 335 ? 10.0 : 10.0 * (x - 150) / 185.0);
        return x >= 1680 ? 0.0 : 10.0 * (1680 - x) / 175.0;
    }
    return 0.0;
}

struct Result { int ticks = 0, correct = 0, stackedTicks = 0, stackedCorrect = 0, flips = 0; };

// One drive. newAlgo=false reproduces the pre-2026-09-26 selection.
static Result drive(bool takeElevated, bool newAlgo, unsigned seed, const Env &env) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n01(0, 1);
    Path path = makePath(takeElevated);
    TrackContinuity tc;
    int cur = 0; // starts matched on the first G segment (unambiguous)
    char lastRoad = 'G';
    double bx = 0, by = 0;
    Result r;
    for (size_t k = 1; k < path.pts.size(); k++) {
        // GPS: white noise + slowly wandering bias (multipath under a viaduct)
        bool under = underViaduct(path.road[k], path.pts[k].first);
        double biasM = under ? env.underBias : env.openBias, noiseM = under ? env.underNoise : env.openNoise;
        // AR(1) bias whose stationary std-dev is biasM
        bx = 0.9 * bx + n01(rng) * biasM * 0.436;
        by = 0.9 * by + n01(rng) * biasM * 0.436;
        double gx = path.pts[k].first + bx + n01(rng) * noiseM;
        double gy = path.pts[k].second + by + n01(rng) * noiseM;
        // GNSS altitude: true height + slowly wandering error, climb over the last ~40 s (2 samples/40 m)
        static double altHist[400];
        static double az = 0;
        az = 0.95 * az + n01(rng) * 0.6;
        altHist[k] = trueAltitude(path.road[k], path.pts[k].first) + az + n01(rng) * 1.0;
        double tx = path.pts[k].first - path.pts[k - 1].first, ty = path.pts[k].second - path.pts[k - 1].second;
        double heading = std::atan2(tx, ty) * 180.0 / M_PI + n01(rng) * 4.0;

        std::vector<TrackCand> cands;
        std::vector<float> confs;
        std::vector<int> idx;
        for (size_t i = 0; i < segs.size(); i++) {
            float conf, d, he;
            if (score(segs[i], gx, gy, heading, conf, d, he)) {
                cands.push_back({segs[i].id, d, he, true, env.gradeFlags ? segs[i].flags : (uint8_t)0});
                confs.push_back(conf);
                idx.push_back((int)i);
            }
        }
        if (cands.empty()) continue;
        int pick = -1;
        if (newAlgo) {
            if (k == 1) { // seed: the car starts matched on the first surface segment
                TrackCand seed = {segs[cur].id, 0, 0, true, 0};
                TrackEvidence e0;
                tc.step(&seed, 1, e0, nbFn, nullptr);
            }
            TrackEvidence ev;
            if (env.altitude && k > 40) { ev.altValid = true; ev.climbM = (float)(altHist[k] - altHist[k - 40]); }
            // sky blocked: detected 85% of the time under the viaduct, 5% false alarms elsewhere
            std::uniform_real_distribution<double> u01(0, 1);
            ev.skyBlocked = env.altitude && (under ? u01(rng) < 0.85 : u01(rng) < 0.05);
            pick = tc.step(cands.data(), (int)cands.size(), ev, nbFn, nullptr);
        } else {
            int best = 0, curI = -1;
            for (size_t i = 0; i < cands.size(); i++) {
                if (confs[i] > confs[best]) best = (int)i;
                if (idx[i] == cur) curI = (int)i;
            }
            bool turning = curI >= 0 && idx[best] != cur &&
                           (confs[best] > confs[curI] + 0.08f || confs[curI] < 0.40f);
            pick = (curI >= 0 && !turning && confs[curI] + 0.1f >= confs[best] - 0.15f) ? curI : best;
        }
        if (pick < 0) continue;
        cur = idx[pick];
        char truth = path.road[k];
        char got = segs[cur].road;
        if (getenv("TC_DEBUG") && newAlgo && truth != got) {
            static int shown = 0;
            if (shown++ < 400) printf("    x=%6.0f truth=%c got=%c(seg %u) gps=(%.1f,%.1f)\n", path.pts[k].first, truth, got, segs[cur].id, gx, gy);
        }
        bool ok = got == truth || (truth == 'G' && got == 'C' && false);
        r.ticks++;
        r.correct += ok;
        double x = path.pts[k].first;
        if (x > 340 && x < 1490) { r.stackedTicks++; r.stackedCorrect += ok; }
        if ((got == 'E') != (lastRoad == 'E') && (got == 'G' || got == 'E') && (lastRoad == 'G' || lastRoad == 'E'))
            r.flips++;
        if (got == 'G' || got == 'E') lastRoad = got;
    }
    return r;
}

static int failures = 0;
static void check(bool cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

int main() {
    const int kRuns = 200;
    struct Case { const char *name; Env env; double target; };
    const Case cases[] = {
        {"current cards (no bridge flags); realistic GPS: 3 m + 4 m bias open sky, 5 m + 8 m bias under the viaduct",
         {3, 4, 5, 8, false, true}, 85.0},  // measured 88-99% (surface under a 6 m-offset viaduct is the weak case)
        {"rebuilt cards (bridge flags) + altitude; same GPS",
         {3, 4, 5, 8, true, true}, 97.0},   // measured 97.6-100%
        {"stress: 5 m + 10 m drifting bias EVERYWHERE, no flags",
         {5, 10, 5, 10, false, false}, 0.0},
    };
    for (const Case &cs : cases) {
        for (double offset : {6.0, 0.0}) {
            buildNetwork(offset);
            printf("\n=== %s | viaduct %.0f m beside/above the road ===\n", cs.name, offset);
            for (bool elevated : {true, false}) {
                for (bool newAlgo : {false, true}) {
                    Result sum;
                    for (int s = 0; s < kRuns; s++) {
                        Result r = drive(elevated, newAlgo, 1000 + s, cs.env);
                        sum.ticks += r.ticks; sum.correct += r.correct;
                        sum.stackedTicks += r.stackedTicks; sum.stackedCorrect += r.stackedCorrect;
                        sum.flips += r.flips;
                    }
                    double stackedPct = 100.0 * sum.stackedCorrect / sum.stackedTicks;
                    printf("  %-24s %s: correct on the stacked stretch %5.1f%%, overall %5.1f%%, layer flips/drive %.2f\n",
                           elevated ? "ON the elevated road" : "UNDER it (surface)", newAlgo ? "NEW" : "OLD",
                           stackedPct, 100.0 * sum.correct / sum.ticks, (double)sum.flips / kRuns);
                    if (newAlgo && cs.target > 0) {
                        char msg[200];
                        snprintf(msg, sizeof(msg), "%s, offset %.0f m: >= %.0f%% correct on the stacked stretch",
                                 elevated ? "on elevated" : "on surface", offset, cs.target);
                        check(stackedPct >= cs.target, msg);
                    }
                }
            }
        }
    }

    // Layer evidence (grade flags + altitude) at acquisition on a stacked stretch.
    printf("\n=== acquisition on a stacked stretch (no history) ===\n");
    {
        TrackCand c[2] = {{1, 2.0f, 5.0f, true, 0}, {2, 3.0f, 5.0f, true, SEGFLAG_BRIDGE}}; // surface marginally closer
        TrackEvidence up; up.altValid = true; up.climbM = 7.0f;
        TrackEvidence down; down.altValid = true; down.climbM = -7.0f;
        TrackEvidence none;
        TrackContinuity a1, a2, a3, a4;
        check(c[a1.step(c, 2, up, nullptr, nullptr)].id == 2, "just climbed 7 m -> picks the bridge");
        check(c[a2.step(c, 2, down, nullptr, nullptr)].id == 1, "just descended 7 m -> picks the surface road");
        check(c[a3.step(c, 2, none, nullptr, nullptr)].id == 1, "no altitude evidence -> closest segment");
        TrackCand g[2] = {{1, 2.0f, 5.0f, true, 0}, {2, 3.0f, 5.0f, true, 0}}; // old card: no grade flags
        check(g[a4.step(g, 2, up, nullptr, nullptr)].id == 1, "no grade flags in data -> altitude is ignored");
    }
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
