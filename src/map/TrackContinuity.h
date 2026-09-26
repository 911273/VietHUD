#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// TrackContinuity — which road segment is the car on, when several are close?
// (HMM / Viterbi map matching, after Newson & Krumm 2009, sized for an MCU.)
//
// The per-fix scorer only sees distance + heading. On stacked roads — an
// elevated ring road right above (or a few metres beside) the surface road it
// follows — both candidates have the same heading and lie within GPS error of
// each other, so any per-fix choice flips between layers or locks onto the
// wrong one at the ramp.
//
// This tracker keeps up to kMaxHyp competing HYPOTHESES ("still on the surface
// road", "took the ramp up", ...). Every fix, each hypothesis may only move to
// segments TOPOLOGICALLY CONNECTED to its own (the neighbour callback: segments
// sharing a node, up to two hops) — an elevated road and the road beneath it
// share no nodes, they meet only through ramps. Each hypothesis accumulates a
// cost: GPS distance + heading error of the segment it claims (Gaussian
// emission), plus a large penalty for a non-connected jump (so recovery after
// bad GPS stays possible). The lowest-cost hypothesis is the answer.
//
// Why this solves the flyover case: the evidence gathered while the car was on
// the ramp (where the two layers are metres apart) stays in the accumulated
// cost afterwards, even where the elevated road lies exactly over the surface
// road and the fixes can no longer tell them apart.
//
// Optional layer evidence: with grade flags in the data (SEGFLAG_BRIDGE /
// TUNNEL) a recent GNSS altitude climb/descent adds cost to the wrong layer.
// Pure logic, no Arduino: host-tested in test/host/test_trackcontinuity.cpp.
// ---------------------------------------------------------------------------

struct TrackCand {
    uint32_t id;
    float distM;      // GPS fix to segment distance
    float headErrDeg; // heading error vs the segment's allowed direction(s), 0..180
    bool headValid;   // heading error meaningful this fix
    uint8_t flags;    // SEGFLAG_*
};

struct TrackEvidence {
    bool altValid = false; // altitude trend available
    float climbM = 0;      // altitude change over the last ~40 s while moving (+ = went up)
    // Sky view suddenly worse than the recent open-sky baseline (HDOP up / used
    // satellites down) — the signature of driving UNDER a viaduct.
    bool skyBlocked = false;
};

// Fills `out` with the ids of segments connected to `segId` (sharing a node,
// up to two hops, either direction). Returns the count.
typedef int (*TrackNeighborFn)(uint32_t segId, uint32_t *out, int maxOut, void *ctx);

class TrackContinuity {
public:
    static const int kMaxHyp = 8;           // hypotheses carried between fixes
    static const int kMaxNeighbors = 48;    // per hypothesis segment
    static const int kNbCache = 16;         // neighbour lists cached (segments change ~1/s)
#ifndef TC_SIGMA_DIST
#define TC_SIGMA_DIST 10.0f
#endif
#ifndef TC_JUMP_COST
#define TC_JUMP_COST 12.0f
#endif
#ifndef TC_OUT_HYST
#define TC_OUT_HYST 1.5f
#endif
    static constexpr float kSigmaDistM = TC_SIGMA_DIST;   // GPS position error model
    static constexpr float kSigmaHeadDeg = 25.0f;
    static constexpr float kHeadCostCap = 6.0f;  // perpendicular roads: bounded, not infinite
    static constexpr float kJumpCost = TC_JUMP_COST;     // moving to a non-connected segment
    static constexpr float kBeam = 20.0f;        // drop hypotheses this much worse than the best
    static constexpr float kOutputHyst = TC_OUT_HYST;   // reported segment changes only for a clearer winner
    static constexpr float kClimbM = 6.0f;       // altitude change that counts as a ramp climb/descent
    static constexpr float kLayerCost = 2.0f;    // cost on the layer the altitude says we are NOT on
    static constexpr float kSkyCost = 1.5f;      // cost on a bridge candidate while the sky is blocked

    TrackContinuity() { reset(); }
    void reset();

    // One GPS fix. Returns the index into c[] of the reported segment, or -1.
    int step(const TrackCand *c, int n, const TrackEvidence &ev, TrackNeighborFn nb, void *ctx,
             const char **reason = nullptr);

    uint32_t reportedId() const { return outId_; }
    int hypothesisCount() const { return nHyp_; }
    // The runner-up's cost margin behind the reported hypothesis (large = sure).
    float margin() const { return margin_; }

private:
    struct Hyp {
        uint32_t id;
        float cost;
    };
    struct NbEntry {
        uint32_t id;
        int n;
        uint32_t ids[kMaxNeighbors];
        uint32_t lastUse;
    };
    const NbEntry *neighbors(uint32_t id, TrackNeighborFn nb, void *ctx);
    float emission(const TrackCand &c, const TrackEvidence &ev, bool mixedLayers) const;

    Hyp hyp_[kMaxHyp];
    int nHyp_;
    uint32_t outId_;
    float margin_;
    NbEntry nb_[kNbCache];
    uint32_t tick_;
};
