#include "TrackContinuity.h"
#include "SpeedMapFormat.h" // SEGFLAG_*

void TrackContinuity::reset() {
    nHyp_ = 0;
    outId_ = 0;
    margin_ = 0;
    tick_ = 0;
    for (int i = 0; i < kNbCache; i++) {
        nb_[i].id = 0;
        nb_[i].n = 0;
        nb_[i].lastUse = 0;
    }
}

const TrackContinuity::NbEntry *TrackContinuity::neighbors(uint32_t id, TrackNeighborFn nb, void *ctx) {
    int lru = 0;
    for (int i = 0; i < kNbCache; i++) {
        if (nb_[i].id == id && id != 0) {
            nb_[i].lastUse = tick_;
            return &nb_[i];
        }
        if (nb_[i].lastUse < nb_[lru].lastUse) lru = i;
    }
    NbEntry &e = nb_[lru];
    e.id = id;
    e.lastUse = tick_;
    e.n = nb ? nb(id, e.ids, kMaxNeighbors, ctx) : 0;
    if (e.n < 0) e.n = 0;
    return &e;
}

static int layerOf(uint8_t flags) {
    if (flags & SEGFLAG_BRIDGE) return 1;
    if (flags & SEGFLAG_TUNNEL) return -1;
    return 0;
}

float TrackContinuity::emission(const TrackCand &c, const TrackEvidence &ev, bool mixedLayers) const {
    float z = c.distM / kSigmaDistM;
    float cost = 0.5f * z * z;
    if (c.headValid) {
        float h = c.headErrDeg / kSigmaHeadDeg;
        float hc = 0.5f * h * h;
        cost += hc > kHeadCostCap ? kHeadCostCap : hc;
    }
    // Layer evidence only between candidates on DIFFERENT layers, and only when
    // the data says which is which (old cards: every segment is layer 0).
    if (mixedLayers && ev.altValid) {
        int layer = layerOf(c.flags);
        if (ev.climbM >= kClimbM && layer <= 0) cost += kLayerCost;
        if (ev.climbM <= -kClimbM && layer > 0) cost += kLayerCost;
    }
    if (mixedLayers && ev.skyBlocked && layerOf(c.flags) > 0) cost += kSkyCost;
    return cost;
}

int TrackContinuity::step(const TrackCand *c, int n, const TrackEvidence &ev, TrackNeighborFn nb, void *ctx,
                          const char **reason) {
    auto say = [&](const char *r) {
        if (reason) *reason = r;
    };
    tick_++;
    if (n <= 0) {
        say("no-candidates");
        return -1; // keep the hypotheses: a gap in candidates is not evidence
    }
    bool mixed = false;
    for (int i = 1; i < n && !mixed; i++)
        if (layerOf(c[i].flags) != layerOf(c[0].flags)) mixed = true;

    static const int kMaxC = 96;
    if (n > kMaxC) n = kMaxC;
    float emit[kMaxC];
    for (int i = 0; i < n; i++) emit[i] = emission(c[i], ev, mixed);

    // Viterbi step: every candidate's best predecessor among the hypotheses.
    float best[kMaxC];
    const bool first = nHyp_ == 0;
    for (int i = 0; i < n; i++) best[i] = first ? emit[i] : 1e30f;
    if (!first) {
        for (int h = 0; h < nHyp_; h++) {
            const NbEntry *e = neighbors(hyp_[h].id, nb, ctx);
            for (int i = 0; i < n; i++) {
                bool conn = c[i].id == hyp_[h].id;
                for (int k = 0; k < e->n && !conn; k++)
                    if (e->ids[k] == c[i].id) conn = true;
                float v = hyp_[h].cost + (conn ? 0.0f : kJumpCost) + emit[i];
                if (v < best[i]) best[i] = v;
            }
        }
    }

    // Keep the kMaxHyp cheapest, within the beam, normalised to min = 0.
    float minCost = 1e30f;
    for (int i = 0; i < n; i++)
        if (best[i] < minCost) minCost = best[i];
    Hyp next[kMaxHyp];
    int nNext = 0;
    int nextIdx[kMaxHyp];
    for (int i = 0; i < n; i++) {
        float v = best[i] - minCost;
        if (v > kBeam) continue;
        bool dup = false; // several candidate slots can carry the same id (tile overlap): keep the cheaper
        for (int k = 0; k < nNext; k++)
            if (next[k].id == c[i].id) {
                if (v < next[k].cost) { next[k].cost = v; nextIdx[k] = i; }
                dup = true;
                break;
            }
        if (dup) continue;
        if (nNext < kMaxHyp) {
            next[nNext] = {c[i].id, v};
            nextIdx[nNext] = i;
            nNext++;
        } else {
            int worst = 0;
            for (int k = 1; k < nNext; k++)
                if (next[k].cost > next[worst].cost) worst = k;
            if (v < next[worst].cost) {
                next[worst] = {c[i].id, v};
                nextIdx[worst] = i;
            }
        }
    }
    for (int k = 0; k < nNext; k++) hyp_[k] = next[k];
    nHyp_ = nNext;

    // Report: the cheapest hypothesis, but keep the previously reported
    // segment's line unless the new winner is clearly (kOutputHyst) better —
    // avoids flicker between two parallel hypotheses with near-equal cost.
    int bestK = 0;
    for (int k = 1; k < nHyp_; k++)
        if (hyp_[k].cost < hyp_[bestK].cost) bestK = k;
    int keepK = -1;
    if (outId_) {
        const NbEntry *e = neighbors(outId_, nb, ctx);
        // the hypothesis continuing the reported one: same id or a connected successor
        for (int k = 0; k < nHyp_; k++) {
            bool cont = hyp_[k].id == outId_;
            for (int j = 0; j < e->n && !cont; j++)
                if (e->ids[j] == hyp_[k].id) cont = true;
            if (cont && (keepK < 0 || hyp_[k].cost < hyp_[keepK].cost)) keepK = k;
        }
    }
    int chosenK = bestK;
    if (keepK >= 0 && hyp_[keepK].cost <= hyp_[bestK].cost + kOutputHyst) chosenK = keepK;
    say(chosenK == bestK ? "best" : "hold-line");

    float second = 1e30f;
    for (int k = 0; k < nHyp_; k++)
        if (k != chosenK && hyp_[k].cost < second) second = hyp_[k].cost;
    margin_ = (second >= 1e29f) ? 99.0f : second - hyp_[chosenK].cost;
    outId_ = hyp_[chosenK].id;
    return nextIdx[chosenK];
}
