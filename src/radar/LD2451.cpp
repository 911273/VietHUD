#include "LD2451.h"
#include "core/AppConfig.h"
#include "core/SharedState.h"
#include "pincfg.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <string.h>

// Datasheet-confirmed default; module is factory-set to this (Table6, V1.03
// doc: "The factory default is 0x0005, that is, 115200"). Unlike the GNSS
// M10N breakout (which turned out NOT to be 9600 despite that being the
// common default — verify, don't just assume), this one is stated
// explicitly by the manufacturer for this exact module, not inferred.
static const uint32_t kRadarBaud = 115200;

// ---------------------------------------------------------------------
// Report frame parser (data output protocol, datasheet section 1.3)
//
// Frame: F4 F3 F2 F1 | length(2, LE) | targetQty(1) alarm(1) [5 bytes/target]*N | F8 F7 F6 F5
// Per-target 5 bytes: angle(1, actual = raw-0x80 deg) distance(1, m)
//                     direction(1, 0=close/1=away) speed(1, km/h) snr(1)
// No checksum — framing is entirely header/length/tail. A byte-by-byte
// state machine (not a line/sentence parser like the GNSS NMEA one) since
// this is a fixed binary framing, not delimited text.
// ---------------------------------------------------------------------
static const uint8_t kReportHeader[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t kReportTail[4] = {0xF8, 0xF7, 0xF6, 0xF5};
static const uint16_t kMaxIntraLen = 2 + 20 * 5; // generous vs. MAX_TARGETS=5 in case a module firmware ever reports more; only the first 5 get used

enum ParseState { P_HDR, P_LEN_LO, P_LEN_HI, P_DATA, P_TAIL };
static ParseState parseState = P_HDR;
static uint8_t hdrMatch = 0;
static uint16_t frameLen = 0;
static uint8_t frameBuf[kMaxIntraLen];
static uint16_t frameIdx = 0;
static uint8_t tailMatch = 0;

static uint32_t g_framesParsed = 0;
static uint32_t g_parseErrors = 0;
static uint32_t g_lastFrameMs = 0;
static uint8_t g_lastQty = 0, g_lastAlarm = 0;

// Relation is still a stand-in for a real Lane Model (Phase 5 doesn't exist
// yet, same reasoning AppConfig.h gives for not having a full lane-corridor
// config) — this only exists so TTC/primary-target selection (which
// requires SAME_LANE, spec section 11) has something to work with against
// real radar data. Upgraded 2026-09-14 from a fixed +/-10 degree angle cone
// to a lateral-OFFSET check (distanceM * sin(angleDeg) vs. cfg.laneHalfWidthM):
// a fixed angle cone implies a lane that gets physically wider the farther
// away a target is, which isn't how a real lane works; a fixed lateral-
// distance threshold stays a constant physical width at any range, same
// idea as the Dashboard's own on-screen lateral placement (which already
// used this formula for target X position, just not for classification).
// Angle sign convention (which side is physically positive) is still
// UNVERIFIED against real hardware — confirm once a real target is visible
// on a known side and correct the ADJACENT_LEFT/RIGHT branch below if
// backwards; that's independent of this lateral-distance fix.
// The actual classification function (classifyRelation()) is defined below
// the Track struct, since it now takes a Track& — see that struct's
// `relation` field comment for why.
// ---------------------------------------------------------------------
// Multi-target persistence/smoothing tracker (spec section 9.1/9.2), added
// 2026-09-15. Every field the LD2451 reports per target (angle, distance,
// speed) is a raw single-byte reading with no persistent ID attached — the
// module just lists "whatever it currently sees," in whatever order its own
// firmware picked that frame. Feeding that straight into RadarSnapshot (as
// this file did before this tracker existed) had three real, user-visible
// effects on the Dashboard: distance/speed jumped a full raw unit (1 m /
// 1 km/h) every single frame instead of changing smoothly; a target that
// dropped out for even one frame (a real, observed occurrence — the module's
// own report cadence isn't perfectly regular) vanished and reappeared,
// flickering; and if two targets were present, a frame where the module
// happened to list them in swapped order made the Dashboard's target dot
// and primary-target selection "teleport" between two different physical
// vehicles for no physical reason. This tracker fixes all three: it matches
// each frame's raw detections against the previous frame's tracked targets
// by physical proximity (not list position), keeps each match in the same
// output slot for its whole lifetime (stable ID), low-pass filters
// distance/angle/speed per track instead of passing raw values straight
// through, and only exposes a track once it's matched several consecutive
// frames (spec 9.1's "2-3 frame" confirm) while tolerating brief gaps before
// actually dropping it (spec 9.1's "500ms" loss window).
//
// Deliberately a simple greedy nearest-neighbor associator, not a Kalman
// filter or Hungarian-algorithm optimal assignment — spec section 9.2 says
// explicitly "Không cần Machine Learning cho V1," and with at most 5 targets
// a greedy match is cheap and good enough; it can still occasionally swap
// identity if two targets cross paths at nearly the same range/angle, a
// known limitation of this class of tracker that a real V1 doesn't need to
// solve.
struct Track {
    bool active = false;
    bool confirmed = false;
    uint16_t hits = 0;       // consecutive matched frames since this track started
    uint32_t lastSeenMs = 0; // last frame this track was matched to a real detection

    // Stable virtual track ID (target-tracking requirement #1, 2026-09-21)
    // — distinct from this struct's own array slot. The slot already
    // happened to stay fixed for a track's whole lifetime (association
    // always updates the SAME slot in place, never reorders), so in
    // practice slot==identity already — this field makes that an explicit,
    // documented guarantee rather than an implicit side effect of the
    // array layout, and gives a real, never-reused number a diagnostics
    // view or log could key on even if the slot itself gets recycled by a
    // later, unrelated target.
    uint32_t trackId = 0;

    // Independently-filtered position state (requirement #9: "smooth
    // distance, x and y independently" — each its own EMA channel against
    // its own raw measurement history, deliberately NOT derived from each
    // other post-filter). distanceM is the filtered radial range; lateralM/
    // longitudinalM are the filtered Cartesian offset (x=lateral/right+,
    // y=longitudinal/forward — same convention the old toLateralLongitudinal()
    // helper already used). Because these three are smoothed independently,
    // sqrt(lateralM^2+longitudinalM^2) need not exactly equal distanceM —
    // that's the accepted tradeoff of filtering each channel on its own
    // terms instead of forcing polar/Cartesian consistency after the fact.
    float distanceM = 0, lateralM = 0, longitudinalM = 0;
    float closingSpeedMps = 0; // filtered relative RADIAL velocity (Doppler) — still needed for TTC/audio-gate/association
    float snr = 0;             // raw, from the most recent matching detection — diagnostic only

    // Per-axis velocity estimate, m/s (requirement #5: "predict target
    // position during short measurement gaps") — a lightweight smoothed
    // rate-of-change of the FILTERED lateral/longitudinal position, same
    // "moderate EMA on an instantaneous derivative" pattern
    // closingAccelMps2 below already uses, not a full Kalman/alpha-beta
    // state (the spec explicitly allows plain EMA). Used by updateTracks()
    // to dead-reckon lateralM/longitudinalM forward while a track goes
    // briefly unmatched, instead of leaving its position frozen at the
    // last real measurement.
    float lateralVelMps = 0, longitudinalVelMps = 0;

    // Anchor position for the prediction above: the last REAL (matched,
    // not predicted) lateralM/longitudinalM plus the time it was measured.
    // Every unmatched tick recomputes lateralM/longitudinalM fresh from
    // this anchor + elapsed time, rather than repeatedly integrating a
    // predicted value into itself tick-over-tick (which would accumulate
    // floating-point/velocity-noise drift over a multi-tick gap).
    float lastMatchedLateralM = 0, lastMatchedLongitudinalM = 0;

    // Smoothed d(closingSpeedMps)/dt estimate (harsh-brake detection, added
    // 2026-09-16 — see core/AppConfig.h's harshBrakeAccelMps2 comment).
    // Computed each match from the RAW new detection against the track's
    // pre-update (still-old) filtered closingSpeedMps, deliberately NOT from
    // two already-EMA-filtered values either side — differencing two lagged
    // signals would double-lag the derivative and blunt exactly the sudden
    // change this is meant to catch. The result is itself lightly smoothed
    // (see updateTracks()) since a single-frame instantaneous derivative off
    // one noisy raw sample is too twitchy to threshold directly.
    float closingAccelMps2 = 0;

    // Hysteresis-classified SAME_LANE/ADJACENT_* state, persisted per track
    // (not recomputed stateless every frame) — see classifyRelation()'s own
    // comment below for why (2026-09-21, user-reported "khong bi nhap nhay
    // lien tuc"). Starts UNKNOWN so the very first classification uses the
    // plain (non-hysteresis) threshold.
    Relation relation = UNKNOWN;
};
static Track tracks[MAX_TARGETS];

// Monotonic counter backing Track::trackId above — never reused, unlike
// the array slot it's assigned alongside.
static uint32_t nextTrackId = 1;

// Sticky primary-target lock — see its own comment at the selection site in
// processReportFrame() below. Reset to -1 wherever tracks[] itself gets
// wiped (radarTaskFn's offline-transition block) so a stale slot index can
// never "lock onto" a completely unrelated track that happens to reuse the
// same array slot after a link recovery.
static int lastPrimarySlot = -1;

// Takes the Track itself (not just angle/distance), and writes back into
// tr.relation with hysteresis, instead of returning a fresh stateless
// classification — added 2026-09-21 (user-reported "theo doi xe phia truoc
// cung lan ... de khong bi nhap nhay lien tuc"). A target sitting right at
// the lane boundary would otherwise flip SAME_LANE/ADJACENT every single
// frame on ordinary residual jitter in the already-EMA-filtered position
// (real hardware noise doesn't vanish just because it's smoothed, it's
// only reduced) — and every flip OUT of SAME_LANE, even for one frame,
// made that track briefly ineligible as the primary target, flickering the
// Dashboard's distance readout/TTC/warning cells and both flash overlays.
// Once classified SAME_LANE, require drifting kHysteresisM further out
// before actually leaving it; entering still uses the plain threshold, so a
// target genuinely changing lanes is still caught promptly. Reads
// tr.lateralM directly now (the independently-filtered channel) instead of
// recomputing distance*sin(angle) — one less trig round-trip, and it's the
// more direct signal for "how far off to the side is this thing."
static void classifyRelation(Track &tr) {
    const float kHysteresisM = 0.3f; // ~30cm — absorbs residual EMA jitter without masking a real lane change
    float threshold = (tr.relation == SAME_LANE) ? (cfg.laneHalfWidthM + kHysteresisM) : cfg.laneHalfWidthM;
    tr.relation = (fabsf(tr.lateralM) <= threshold) ? SAME_LANE : (tr.lateralM < 0 ? ADJACENT_LEFT : ADJACENT_RIGHT);
}

// Max frame-to-frame physical movement (lateral+longitudinal, meters) still
// counted as "the same" target for data association. Not currently a
// Settings tunable — spec 9.1 only requires the persistence TIMING values
// above to be tunable, not this gate. 15m is a rough estimate (closing speed
// up to ~200 km/h relative = ~56 m/s, against an assumed worst-case ~250ms
// gap between per-target reports) since the LD2451's own report interval
// while actively tracking a target isn't documented or measured yet on real
// hardware (only the much slower ~1.2-1.5s *idle*/no-target cadence has
// been observed — see radarTaskFn's kFrameTimeoutMs comment); revisit this
// once a real report-rate measurement exists.
static const float kTrackGateM = 15.0f;

static inline void toLateralLongitudinal(float distanceM, float angleDeg, float &lateral, float &longitudinal) {
    float rad = angleDeg * (float)M_PI / 180.0f;
    lateral = distanceM * sinf(rad);      // same convention as classifyRelation()
    longitudinal = distanceM * cosf(rad);
}

struct RawDetection {
    float distanceM, angleDeg, closingSpeedMps, snr;
};

// Association cost also factors in relative-velocity difference now
// (target-tracking requirement #3: "distance + x/y/angle + relative
// velocity"), not position alone — weighted into the SAME meter-scale unit
// posCost already uses (not a separate unitless term) so the two combine
// on one sensible scale: kVelCostWeightS m/s of speed mismatch behaves like
// this many meters of position mismatch. Picked so a genuinely different
// vehicle with a wildly different closing speed at a similar position (two
// vehicles momentarily overlapping in range/angle) is less likely to steal
// an existing track's identity than one that's actually moving the same
// way, while a single real vehicle's own Doppler measurement noise
// (typically well under 1 m/s frame-to-frame) barely moves the total cost.
static const float kVelCostWeightS = 0.5f;

// Associates this frame's raw detections with existing tracks, ages out
// unmatched tracks (predicting their position forward through a short gap
// instead of freezing it — requirement #5), and starts new provisional
// ones — called once per received frame, including "no target" (nDets==0)
// frames, since those are exactly what the 1-frame-miss/500ms-loss rules
// need to age existing tracks against.
static void updateTracks(const RawDetection *dets, int nDets, uint32_t nowMs) {
    bool detUsed[MAX_TARGETS] = {false};
    bool trackMatched[MAX_TARGETS] = {false};

    // Greedy nearest-neighbor: repeatedly pick the closest still-unmatched
    // (track, detection) pair within the gate until none remain. Tracks
    // associate from their CURRENT lateralM/longitudinalM — which, for a
    // track that missed a frame last tick, is already the gap-predicted
    // position (see below), not a stale last-measured spot.
    for (;;) {
        float bestCost = kTrackGateM;
        int bestTrack = -1, bestDet = -1;
        for (int t = 0; t < MAX_TARGETS; t++) {
            if (!tracks[t].active || trackMatched[t]) continue;
            for (int d = 0; d < nDets; d++) {
                if (detUsed[d]) continue;
                float dLat, dLon;
                toLateralLongitudinal(dets[d].distanceM, dets[d].angleDeg, dLat, dLon);
                float posCost = hypotf(dLat - tracks[t].lateralM, dLon - tracks[t].longitudinalM);
                float velCost = fabsf(dets[d].closingSpeedMps - tracks[t].closingSpeedMps) * kVelCostWeightS;
                float cost = posCost + velCost;
                if (cost < bestCost) {
                    bestCost = cost;
                    bestTrack = t;
                    bestDet = d;
                }
            }
        }
        if (bestTrack < 0) break;

        // A track's very first sample is seeded directly at creation time
        // below (hits=1, no blending needed — same pattern as GNSS.cpp's
        // SpeedFilter emaInit), so every match reaching this point is at
        // least this track's second sample: always EMA-blend.
        Track &tr = tracks[bestTrack];
        const RawDetection &d = dets[bestDet];
        float dLat, dLon;
        toLateralLongitudinal(d.distanceM, d.angleDeg, dLat, dLon);

        // Harsh-brake accel estimate — computed BEFORE tr.closingSpeedMps is
        // overwritten below, against the raw new detection (see
        // Track::closingAccelMps2's comment on why not two filtered values).
        // dtS bounds: >0.02s rejects a same-tick double-match (shouldn't
        // happen, cheap to guard anyway); <2.0s rejects a stale track that
        // just barely survived the loss window, where the elapsed gap alone
        // would swamp the estimate with a meaningless huge/tiny value. The
        // same dtS also backs the per-axis velocity estimate just below,
        // for the gap-prediction requirement.
        float dtS = (nowMs - tr.lastSeenMs) / 1000.0f;
        if (tr.hits > 0 && dtS > 0.02f && dtS < 2.0f) {
            float instAccel = (d.closingSpeedMps - tr.closingSpeedMps) / dtS;
            const float accelBeta = 0.5f; // moderate smoothing — enough to reject single-sample noise, not so much it blunts a real sudden change
            tr.closingAccelMps2 = accelBeta * instAccel + (1.0f - accelBeta) * tr.closingAccelMps2;

            // Per-axis velocity (requirement #5's prediction feed) — against
            // the track's own PRE-update lateralM/longitudinalM (which, if
            // last tick was a miss, is already that tick's predicted
            // position — differencing against it still yields a sane rate
            // once a real detection resumes matching).
            float instLatVel = (dLat - tr.lateralM) / dtS;
            float instLonVel = (dLon - tr.longitudinalM) / dtS;
            const float velBeta = 0.5f;
            tr.lateralVelMps = velBeta * instLatVel + (1.0f - velBeta) * tr.lateralVelMps;
            tr.longitudinalVelMps = velBeta * instLonVel + (1.0f - velBeta) * tr.longitudinalVelMps;
        }

        // Independent per-channel EMA (requirement #9) — distanceM/lateralM/
        // longitudinalM each filtered straight from their OWN raw
        // measurement, not derived from one another, so residual
        // disagreement between e.g. distanceM and hypot(lateralM,longitudinalM)
        // is expected, not a bug (see Track's own field comment).
        float alpha = cfg.radarTrackFilterAlpha;
        tr.distanceM = alpha * d.distanceM + (1.0f - alpha) * tr.distanceM;
        tr.lateralM = alpha * dLat + (1.0f - alpha) * tr.lateralM;
        tr.longitudinalM = alpha * dLon + (1.0f - alpha) * tr.longitudinalM;
        tr.closingSpeedMps = alpha * d.closingSpeedMps + (1.0f - alpha) * tr.closingSpeedMps;
        tr.snr = d.snr;
        tr.lastSeenMs = nowMs;
        tr.lastMatchedLateralM = tr.lateralM; // anchor for the NEXT gap's prediction, should this track miss a future frame
        tr.lastMatchedLongitudinalM = tr.longitudinalM;
        if (tr.hits < 0xFFFF) tr.hits++;
        if (tr.hits >= (uint16_t)cfg.radarTrackConfirmFrames) tr.confirmed = true;
        classifyRelation(tr); // uses the just-updated filtered lateralM above, with hysteresis against tr's own previous relation

        trackMatched[bestTrack] = true;
        detUsed[bestDet] = true;
    }

    // Unmatched active tracks: keep through a brief gap (spec: lose 1 frame,
    // keep target), only actually drop once the configured loss window
    // elapses with no match at all. While still within the window, predict
    // the position forward from the last REAL match using the track's own
    // velocity estimate (requirement #5) — recomputed fresh from the
    // matched anchor + elapsed gap each tick (not integrated tick-over-tick)
    // so a multi-tick gap doesn't accumulate drift from re-adding the same
    // velocity noise repeatedly. distanceM during a predicted tick falls
    // back to the Pythagorean-consistent value (no independent raw
    // distance sample exists to keep smoothing this tick) — the
    // independent-channel filtering above only applies to REAL matches.
    uint32_t lossMs = (uint32_t)(cfg.radarTrackLossS * 1000.0f);
    for (int t = 0; t < MAX_TARGETS; t++) {
        if (!tracks[t].active || trackMatched[t]) continue;
        uint32_t gapMs = nowMs - tracks[t].lastSeenMs;
        if (gapMs > lossMs) {
            tracks[t] = Track(); // drop: frees this slot for a new track
            continue;
        }
        float gapS = gapMs / 1000.0f;
        tracks[t].lateralM = tracks[t].lastMatchedLateralM + tracks[t].lateralVelMps * gapS;
        tracks[t].longitudinalM = tracks[t].lastMatchedLongitudinalM + tracks[t].longitudinalVelMps * gapS;
        tracks[t].distanceM = hypotf(tracks[t].lateralM, tracks[t].longitudinalM);
        classifyRelation(tracks[t]); // re-classify against the predicted position too — a coasting track can still drift across the lane boundary
    }

    // Unmatched detections: start a new provisional (unconfirmed) track in
    // any free slot. If every slot is already occupied, the detection is
    // simply not tracked this frame — spec 9.2's "hỗ trợ tối đa 5 target".
    for (int d = 0; d < nDets; d++) {
        if (detUsed[d]) continue;
        for (int t = 0; t < MAX_TARGETS; t++) {
            if (tracks[t].active) continue;
            tracks[t] = Track();
            tracks[t].active = true;
            tracks[t].trackId = nextTrackId++;
            tracks[t].distanceM = dets[d].distanceM;
            toLateralLongitudinal(dets[d].distanceM, dets[d].angleDeg, tracks[t].lateralM, tracks[t].longitudinalM);
            tracks[t].lastMatchedLateralM = tracks[t].lateralM;
            tracks[t].lastMatchedLongitudinalM = tracks[t].longitudinalM;
            tracks[t].closingSpeedMps = dets[d].closingSpeedMps;
            tracks[t].snr = dets[d].snr;
            tracks[t].lastSeenMs = nowMs;
            tracks[t].hits = 1;
            classifyRelation(tracks[t]); // seed with the plain (non-hysteresis) threshold — relation starts UNKNOWN
            tracks[t].confirmed = false; // cfg.radarTrackConfirmFrames is clamped >=2, so hits==1 is never enough
            break;
        }
    }
}

static void processReportFrame(const uint8_t *buf, uint16_t len) {
    g_framesParsed++;
    g_lastFrameMs = millis();

    // len==0 is the real, documented "no target" frame (confirmed on real
    // hardware — see the P_LEN_HI comment), not an error: qty=0, alarm=0,
    // no target bytes to read. len==1 genuinely would be malformed (can't
    // fit even qty+alarm), so that's still rejected.
    uint8_t qty = 0, alarm = 0;
    if (len == 1) {
        g_parseErrors++;
        return;
    }
    if (len >= 2) {
        qty = buf[0];
        alarm = buf[1];
    }
    g_lastQty = qty;
    g_lastAlarm = alarm;

    RadarSnapshot radar;
    radar.online = true;

    int usable = qty;
    if (usable > MAX_TARGETS) usable = MAX_TARGETS; // firmware max (spec section 4.1)
    // Defensive: don't read past what the frame actually contains even if
    // qty claims more than len backs up (protects against a corrupt qty
    // byte slipping past the header/tail framing check).
    int fitsInFrame = len >= 2 ? (len - 2) / 5 : 0;
    if (usable > fitsInFrame) usable = fitsInFrame;

    // Decode this frame's raw detections first, THEN hand them to the
    // tracker (updateTracks() above) — raw values never go straight into
    // RadarSnapshot any more (see that function's header comment for why:
    // no persistent ID, no filtering, from the module itself).
    RawDetection dets[MAX_TARGETS];
    for (int i = 0; i < usable; i++) {
        int base = 2 + i * 5;
        uint8_t angleRaw = buf[base];
        uint8_t distanceRaw = buf[base + 1];
        uint8_t dirRaw = buf[base + 2];
        uint8_t speedRaw = buf[base + 3];
        uint8_t snrRaw = buf[base + 4];

        dets[i].angleDeg = (float)((int)angleRaw - 0x80);
        // Mounting-flip correction (see core/AppConfig.h's
        // radarMountFlipped comment) — applied here, at the single point
        // every downstream consumer (tracking, lane classification,
        // Dashboard target placement) ultimately derives its angle from,
        // rather than patched separately in each of them.
        if (cfg.radarMountFlipped) dets[i].angleDeg = -dets[i].angleDeg;
        dets[i].distanceM = (float)distanceRaw;
        float speedKmh = (float)speedRaw;
        dets[i].closingSpeedMps = (dirRaw == 0 ? 1.0f : -1.0f) * (speedKmh / 3.6f); // 0=close(approaching), 1=away
        dets[i].snr = (float)snrRaw;
    }
    updateTracks(dets, usable, millis());

    for (int i = 0; i < MAX_TARGETS; i++) {
        Track &tr = tracks[i];
        SimTarget &t = radar.targets[i];
        if (!tr.active || !tr.confirmed) {
            t.active = false;
            continue;
        }
        t.active = true;
        t.trackId = tr.trackId;
        // angleDeg is DERIVED here purely for the existing consumers that
        // still want it (Dashboard.cpp's "show angle" diagnostic label,
        // WebPortal's JSON) — it is NOT itself an independently-filtered
        // channel (requirement #9 only asks for distance/x/y to be
        // smoothed independently); reconstructing it from the filtered
        // lateralM/longitudinalM keeps those consumers working unchanged
        // without a second, redundant angle filter.
        t.angleDeg = atan2f(tr.lateralM, tr.longitudinalM) * (180.0f / (float)M_PI);
        t.lateralM = tr.lateralM; // the real, independently-filtered channel — Dashboard.cpp's on-screen X uses this directly now, not distance*sin(angle)
        t.distanceM = tr.distanceM;
        t.closingSpeedMps = tr.closingSpeedMps;
        // SNR (0-255 range per datasheet) isn't calibrated against a known
        // good/bad threshold yet — no real-world data to tune against.
        // Fixed baseline confidence instead of an unverified SNR formula,
        // same "don't fabricate precision you don't have" reasoning as
        // GNSS's speed filter alpha being a real tunable, not a guess.
        t.confidence = 0.85f;
        t.snr = tr.snr;
        t.relation = tr.relation; // already hysteresis-classified by classifyRelation(), called from updateTracks() above
        t.ttcS = (t.closingSpeedMps <= 0.05f) ? INFINITY : (t.distanceM / t.closingSpeedMps);
        t.closingAccelMps2 = tr.closingAccelMps2;
        // Tailgating (spec section 33's minDistanceM — see core/AppConfig.h's
        // comment): distance-only, deliberately independent of ttcS/closing
        // speed above, so a SAME_LANE target that's simply too close but not
        // currently closing (stop-and-go traffic — ttcS would read INFINITY)
        // still gets flagged.
        t.tooClose = (t.relation == SAME_LANE && t.distanceM < cfg.minDistanceM);
    }

    // Primary-target selection picks the CLOSEST confirmed SAME_LANE track,
    // not "whichever is closing fastest" — changed 2026-09-21 (user-
    // requested "luon hien thi khoang cach voi xe phia truoc tren cung lan
    // duong"). The old `closingSpeedMps > 0` requirement meant the distance
    // readout (and the tailgating tooClose check below, which only ever
    // fires for radar.primaryIdx) went blank/silent the instant the car
    // ahead stopped closing — stop-and-go traffic, cruising at matched
    // speed — exactly the case core/AppConfig.h's minDistanceM/tooClose
    // comment says must still be caught ("ttcS would read INFINITY"), but
    // it never could if that same target was never even selected as
    // primary in the first place. Dropping the closing requirement fixes
    // both: distance now always reflects the real nearest same-lane
    // vehicle, and tooClose now actually fires for it. TTC/warning logic
    // downstream is unaffected in the normal case — ttcS is still INFINITY
    // for a non-closing target, so primaryTtcWarn/the flash overlays still
    // correctly stay quiet; the one real tradeoff is a same-lane vehicle
    // farther away but closing dangerously fast could be passed over for
    // TTC-warning purposes in favor of a closer-but-static one directly
    // ahead — accepted since harshBrakeWarning below already scans EVERY
    // confirmed SAME_LANE target independent of primaryIdx, so a genuinely
    // sudden approach from further back still gets flagged either way.
    //
    // Still STICKY once locked (see the original 2026-09-21 "khong bi nhap
    // nhay lien tuc" fix this replaces the TTC version of) — margin is now
    // in meters, not TTC-seconds, to match the new distance-based
    // criterion: only steal the lock for a candidate meaningfully CLOSER,
    // not just marginally different on ordinary sensor jitter.
    int bestCandidate = -1;
    float bestCandidateDist = 1e9f;
    for (int i = 0; i < MAX_TARGETS; i++) {
        SimTarget &t = radar.targets[i];
        if (t.active && t.relation == SAME_LANE && t.confidence >= cfg.minConfidence &&
            t.distanceM < bestCandidateDist) {
            bestCandidateDist = t.distanceM;
            bestCandidate = i;
        }
    }
    bool lockEligible = lastPrimarySlot >= 0 && radar.targets[lastPrimarySlot].active &&
                         radar.targets[lastPrimarySlot].relation == SAME_LANE &&
                         radar.targets[lastPrimarySlot].confidence >= cfg.minConfidence;
    const float kSwitchMarginM = 2.0f; // steal the lock only for a candidate at least this much closer — see comment above
    if (lockEligible) {
        if (bestCandidate != lastPrimarySlot && bestCandidateDist + kSwitchMarginM < radar.targets[lastPrimarySlot].distanceM) {
            lastPrimarySlot = bestCandidate;
        }
        // else: keep the existing lock even though bestCandidate may point elsewhere this frame
    } else {
        lastPrimarySlot = bestCandidate;
    }
    radar.primaryIdx = lastPrimarySlot;

    // Harsh-braking-ahead (core/AppConfig.h's harshBrakeAccelMps2 comment):
    // checked across every confirmed SAME_LANE target, not just the primary
    // one selected above — a target closing unusually fast is worth
    // flagging even in the one frame before it might become primary.
    radar.harshBrakeWarning = false;
    for (int i = 0; i < MAX_TARGETS; i++) {
        SimTarget &t = radar.targets[i];
        if (t.active && t.relation == SAME_LANE && t.confidence >= cfg.minConfidence &&
            t.closingAccelMps2 > cfg.harshBrakeAccelMps2) {
            radar.harshBrakeWarning = true;
            break;
        }
    }

    GnssSnapshot gnss = gnssSnapshot();
    float audioDisableKmh = cfg.audioEnableKmh - cfg.hysteresisKmh;
    static bool audioSpeedEnabled = false;
    if (gnss.egoSpeedKmh > cfg.audioEnableKmh) audioSpeedEnabled = true;
    else if (gnss.egoSpeedKmh < audioDisableKmh) audioSpeedEnabled = false;
    // Three independent reasons to allow audio, all still gated behind the
    // same speed hysteresis/fix/enabled checks (spec section 13 — the >60
    // km/h rule applies no matter WHY the warning fired, not just for TTC):
    // primary target's TTC below warning threshold (original spec 13 rule),
    // primary target too close regardless of TTC (tailgating, added
    // 2026-09-16), or any SAME_LANE target closing unusually fast (harsh
    // braking, added 2026-09-16).
    bool primaryTtcWarn = radar.primaryIdx >= 0 && radar.targets[radar.primaryIdx].ttcS < cfg.ttcWarnS;
    bool primaryTooClose = radar.primaryIdx >= 0 && radar.targets[radar.primaryIdx].tooClose;
    radar.audioAllowed = cfg.audioEnabled && gnss.fix && audioSpeedEnabled &&
                          (primaryTtcWarn || primaryTooClose || radar.harshBrakeWarning);

    radar.framesParsed = g_framesParsed;
    radar.parseErrors = g_parseErrors;
    radar.lastTargetQty = qty;
    radar.lastAlarm = alarm;
    radar.lastSnr = usable > 0 ? (uint8_t)dets[0].snr : 0; // raw, from this frame directly — diagnostic only, not track-filtered

    radarPublish(radar);
}

static void feedParser(uint8_t b) {
    switch (parseState) {
        case P_HDR:
            if (b == kReportHeader[hdrMatch]) {
                hdrMatch++;
                if (hdrMatch == 4) parseState = P_LEN_LO;
            } else {
                hdrMatch = (b == kReportHeader[0]) ? 1 : 0;
            }
            break;
        case P_LEN_LO:
            frameLen = b;
            parseState = P_LEN_HI;
            break;
        case P_LEN_HI:
            frameLen |= ((uint16_t)b << 8);
            if (frameLen > kMaxIntraLen) {
                g_parseErrors++;
                parseState = P_HDR;
                hdrMatch = 0;
            } else if (frameLen == 0) {
                // Confirmed on real hardware 2026-09-16 (raw byte dump):
                // "no target" frames are header+length(0x0000)+tail with
                // NO qty/alarm bytes at all, not qty=0/alarm=0 as two
                // zero bytes — the datasheet's own prose ("if no target is
                // detected, only the frame header, frame length, and
                // frame tail are output") turned out to be exact, not
                // approximate. Skip straight to the tail, nothing to read.
                frameIdx = 0;
                parseState = P_TAIL;
                tailMatch = 0;
            } else {
                frameIdx = 0;
                parseState = P_DATA;
            }
            break;
        case P_DATA:
            frameBuf[frameIdx++] = b;
            if (frameIdx >= frameLen) {
                parseState = P_TAIL;
                tailMatch = 0;
            }
            break;
        case P_TAIL:
            if (b == kReportTail[tailMatch]) {
                tailMatch++;
                if (tailMatch == 4) {
                    processReportFrame(frameBuf, frameLen);
                    parseState = P_HDR;
                    hdrMatch = 0;
                }
            } else {
                g_parseErrors++;
                parseState = P_HDR;
                hdrMatch = (b == kReportHeader[0]) ? 1 : 0;
            }
            break;
    }
}

// ---------------------------------------------------------------------
// Command/ACK protocol (datasheet section 1.2) — used once at startup to
// push the radar settings already configurable in Settings > Radar
// (spec section 24: "load saved radar configuration -> send configuration
// to LD2451 -> verify response -> start monitoring"). Blocking, with a
// timeout, since it only runs before the main receive loop starts, not
// during normal operation.
// ---------------------------------------------------------------------
static const uint8_t kCmdHeader[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t kCmdTail[4] = {0x04, 0x03, 0x02, 0x01};

static void sendCommand(uint16_t cmdWord, const uint8_t *value, uint8_t valueLen) {
    uint8_t out[4 + 2 + 2 + 32 + 4];
    int n = 0;
    memcpy(out + n, kCmdHeader, 4);
    n += 4;
    uint16_t intraLen = 2 + valueLen;
    out[n++] = intraLen & 0xFF;
    out[n++] = (intraLen >> 8) & 0xFF;
    out[n++] = cmdWord & 0xFF;
    out[n++] = (cmdWord >> 8) & 0xFF;
    for (uint8_t i = 0; i < valueLen; i++) out[n++] = value[i];
    memcpy(out + n, kCmdTail, 4);
    n += 4;
    Serial1.write(out, n);
}

// Blocking wait for the ACK frame matching cmdWord, up to timeoutMs.
// Returns true if the ACK's 2-byte status field is 0 (success).
static bool waitForAck(uint16_t cmdWord, uint32_t timeoutMs) {
    enum { A_HDR, A_LEN_LO, A_LEN_HI, A_DATA, A_TAIL } state = A_HDR;
    uint8_t match = 0, tmatch = 0;
    uint16_t len = 0, idx = 0;
    uint8_t buf[32];
    uint32_t deadline = millis() + timeoutMs;

    while (millis() < deadline) {
        if (!Serial1.available()) {
            delay(2);
            continue;
        }
        uint8_t b = (uint8_t)Serial1.read();
        switch (state) {
            case A_HDR:
                if (b == kCmdHeader[match]) {
                    match++;
                    if (match == 4) state = A_LEN_LO;
                } else {
                    match = (b == kCmdHeader[0]) ? 1 : 0;
                }
                break;
            case A_LEN_LO:
                len = b;
                state = A_LEN_HI;
                break;
            case A_LEN_HI:
                len |= ((uint16_t)b << 8);
                if (len == 0 || len > sizeof(buf)) {
                    state = A_HDR;
                    match = 0;
                } else {
                    idx = 0;
                    state = A_DATA;
                }
                break;
            case A_DATA:
                buf[idx++] = b;
                if (idx >= len) {
                    state = A_TAIL;
                    tmatch = 0;
                }
                break;
            case A_TAIL:
                if (b == kCmdTail[tmatch]) {
                    tmatch++;
                    if (tmatch == 4) {
                        // Table5: ack word (send word | 0x0100, 2 bytes LE) + status (2 bytes LE)
                        if (len >= 4) {
                            uint16_t ackWord = buf[0] | ((uint16_t)buf[1] << 8);
                            uint16_t status = buf[2] | ((uint16_t)buf[3] << 8);
                            if (ackWord == (cmdWord | 0x0100)) return status == 0;
                        }
                        state = A_HDR;
                        match = 0;
                    }
                } else {
                    state = A_HDR;
                    match = (b == kCmdHeader[0]) ? 1 : 0;
                }
                break;
        }
    }
    return false; // timed out
}

// Enable config -> push target-detection params from cfg -> end config.
// Non-fatal if any step fails/times out: just logged, radar keeps running
// on its own current/default settings and data still gets read normally.
static void configureRadar() {
    // Every ACK wait below got at least one retry after real hardware
    // testing 2026-09-16 showed intermittent single-attempt timeouts even
    // with the link otherwise healthy (param push failed once right after
    // a run where End-Config alone needed its 2nd retry) — a plausible
    // shared cause is the touch driver's own I2C bus contention (a
    // recurring theme elsewhere in this project, see TouchTask.cpp)
    // occasionally delaying this task's UART servicing past a single
    // 200-300ms window. Retrying is cheap and this only runs once at boot.
    bool enableOk = false;
    for (int attempt = 0; attempt < 2 && !enableOk; attempt++) {
        if (attempt > 0) delay(50);
        uint8_t enableVal[2] = {0x01, 0x00};
        sendCommand(0x00FF, enableVal, 2);
        enableOk = waitForAck(0x00FF, 300);
    }
    if (!enableOk) {
        Serial.println("[radar] WARN: enable-config ACK failed/timed out, skipping param push");
        return;
    }

    uint8_t maxRange = (uint8_t)constrain((int)cfg.maxRangeM, 0x0A, 0xFF);
    uint8_t direction = (uint8_t)constrain((int)cfg.radarDirection, 0, 2); // Settings > Radar > Direction
    uint8_t minSpeed = (uint8_t)constrain((int)cfg.minTargetSpeedKmh, 0, 0x78);
    uint8_t noTargetDelay = (uint8_t)constrain((int)cfg.radarNoTargetDelayS, 0, 30); // Settings > Radar > No-target delay
    uint8_t paramVal[4] = {maxRange, direction, minSpeed, noTargetDelay};
    bool paramOk = false;
    for (int attempt = 0; attempt < 2 && !paramOk; attempt++) {
        if (attempt > 0) delay(50);
        sendCommand(0x0002, paramVal, 4);
        paramOk = waitForAck(0x0002, 300);
    }
    if (!paramOk) Serial.println("[radar] WARN: target-detection-param ACK failed after retry");

    // Sensitivity (datasheet 1.2.5, command 0x0003) — Settings > Radar >
    // SNR sensitivity / Trigger count. Higher trigger count = fewer false
    // positives but slower to first-report; higher SNR level = less
    // sensitive (fewer weak/distant/marginal detections reported).
    uint8_t triggerCount = (uint8_t)constrain((int)cfg.radarTriggerCount, 1, 0x0A);
    uint8_t snrLevel = (uint8_t)constrain((int)cfg.radarSnrLevel, 0, 8);
    uint8_t sensVal[4] = {triggerCount, snrLevel, 0x00, 0x00};
    bool sensOk = false;
    for (int attempt = 0; attempt < 2 && !sensOk; attempt++) {
        if (attempt > 0) delay(50);
        sendCommand(0x0003, sensVal, 4);
        sensOk = waitForAck(0x0003, 300);
    }
    if (!sensOk) Serial.println("[radar] WARN: sensitivity-param ACK failed after retry");

    // End-Config is the one step worth retrying: the datasheet says report
    // streaming only resumes once it's actually processed ("the radar will
    // resume working mode after execution"), and that was confirmed on
    // real hardware 2026-09-16 — a run where this ACK timed out saw zero
    // report frames for the next 15s straight (module still paused in
    // config mode), while a run where it succeeded started streaming
    // immediately. Getting stuck here silently would mean "no data at
    // all", so it's worth a few attempts rather than one 200ms try.
    bool endOk = false;
    for (int attempt = 0; attempt < 3 && !endOk; attempt++) {
        if (attempt > 0) delay(50);
        sendCommand(0x00FE, NULL, 0);
        endOk = waitForAck(0x00FE, 300);
    }
    if (!endOk) Serial.println("[radar] WARN: end-config ACK failed after 3 attempts — radar may be stuck in config mode");

    Serial.printf("[radar] startup config: maxRange=%dm minSpeed=%dkm/h dir=%d noTgtDelay=%ds trig=%d snr=%d -> "
                  "param=%s sens=%s end=%s\n",
                  maxRange, minSpeed, direction, noTargetDelay, triggerCount, snrLevel, paramOk ? "OK" : "FAIL",
                  sensOk ? "OK" : "FAIL", endOk ? "OK" : "FAIL");
}

// ---------------------------------------------------------------------
static void radarTaskFn(void *) {
    Serial1.begin(kRadarBaud, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
    delay(100); // let the module finish its own boot before talking to it
    configureRadar();

    esp_task_wdt_add(NULL);
    uint32_t lastDebugMs = 0;
    // Radar considered "lost" if no successfully-framed report for this
    // long — spec T14: no fake targets, no fake TTC, radar fault shown.
    // The LD2451 reports periodically even with zero targets (datasheet
    // 1.3: "if no target is detected, only header/length/tail are
    // output"), so this timeout firing means the UART link itself is
    // dead, not just an empty scene.
    //
    // 1000ms was too tight and this actually fired continuously on real
    // hardware (user-reported 2026-09-16 "radar báo fault liên tục") even
    // though the link was perfectly healthy the whole time — confirmed via
    // [radar] debug log: frames were climbing steadily
    // (97->99->101->103->105) with parseErrors=0, but sinceLastFrame was
    // observed reaching 1161ms+ between consecutive idle "no target"
    // frames, i.e. this module's own idle report cadence is genuinely
    // slower than 1s, not something this firmware controls (the "no
    // target delay" config field is a presence-debounce, not the report
    // interval). 3s was picked to cover that.
    //
    // 3000ms turned out to still be too tight once a REAL target was
    // actually being tracked (user-reported 2026-09-21 "khi hoat dong van
    // bi bao radar fault", recurring during normal use, unrelated to the
    // separate demoMode-stuck-on issue found the same day). Two independent
    // 90s real-hardware captures showed sinceLastFrame reaching 3541-3820ms
    // specifically in the frames right after a lastQty=1 (real detection)
    // report, parseErrors=0 throughout both — i.e. the module's own report
    // cadence WHILE actively tracking a target is genuinely slower than the
    // previously-measured idle cadence, not a link problem, and not
    // something this firmware controls. This is worse than the original
    // 1000ms bug in one way: it means the false FAULT flash happens right
    // when a real target is present, the one moment it matters most. 6000ms
    // gives ~57% headroom over the observed 3820ms worst case, same
    // "comfortable margin above a real measured gap" reasoning as before —
    // revisit if a longer real-hardware capture ever shows a larger gap.
    const uint32_t kFrameTimeoutMs = 6000;

    // Tracks a fail-safe transition so the module's saved settings get
    // re-pushed exactly once when the link comes back, not on every healthy
    // frame. Rationale: this firmware's own fail-safe only fires on a real
    // link outage (see kFrameTimeoutMs's comment above), and unlike this
    // ESP32 (which re-runs configureRadar() from scratch on any reset), the
    // LD2451 module itself has no way to tell THIS firmware it lost power or
    // reset independently — if that's what caused the outage, it would
    // silently come back on its own factory-default settings instead of the
    // ones actually configured in Settings > Radar. Re-sending the same
    // config the module already has is a harmless no-op (instant ACK), so
    // this is cheap insurance against the case where it wasn't a no-op.
    bool wasOffline = false;

    for (;;) {
        uint32_t framesBefore = g_framesParsed;
        while (Serial1.available()) {
            feedParser((uint8_t)Serial1.read());
        }
        bool gotFrame = g_framesParsed != framesBefore;

        // No `g_lastFrameMs != 0` guard here (removed 2026-09-16): that guard
        // meant a radar that had NEVER received a single frame this boot
        // (module never plugged in, or unplugged before this boot) could
        // never be declared offline at all, leaving RadarSnapshot stuck on
        // its struct default forever — see SharedState.h's online comment.
        // millis() - 0 already naturally stays under kFrameTimeoutMs for the
        // first 3s after boot on its own, so dropping the guard doesn't
        // cause a false fault flash on startup.
        bool offlineNow = millis() - g_lastFrameMs > kFrameTimeoutMs;
        if (offlineNow) {
            // Drop all in-flight tracks once, on the transition into offline
            // (not every loop iteration — tracks[] is a static array, so this
            // is otherwise wasted work at 20ms/iteration). Without this, a
            // link that drops and comes back could stitch a brand-new,
            // physically unrelated detection onto a stale pre-outage track
            // (same slot, updateTracks() would just see it as "the same
            // target, just gone a while" if it happens to land within
            // kTrackGateM) instead of correctly starting fresh.
            if (!wasOffline) {
                for (int i = 0; i < MAX_TARGETS; i++) tracks[i] = Track();
                lastPrimarySlot = -1; // see its own declaration comment
            }
            wasOffline = true;
            RadarSnapshot offline;
            offline.online = false;
            offline.primaryIdx = -1;
            offline.audioAllowed = false;
            for (int i = 0; i < MAX_TARGETS; i++) offline.targets[i].active = false;
            offline.framesParsed = g_framesParsed;
            offline.parseErrors = g_parseErrors;
            offline.lastTargetQty = g_lastQty;
            offline.lastAlarm = g_lastAlarm;
            offline.lastSnr = 0;
            radarPublish(offline);
        } else if (gotFrame && wasOffline) {
            wasOffline = false;
            Serial.println("[radar] link recovered after a fault — re-pushing saved configuration "
                            "in case the module itself power-cycled");
            configureRadar();
        }

        uint32_t now = millis();
        if (now - lastDebugMs > 3000) {
            lastDebugMs = now;
            RadarSnapshot dbg = radarSnapshot();
            Serial.printf("[radar] frames=%lu parseErrors=%lu lastQty=%u lastAlarm=%u lastSnr=%u sinceLastFrame=%lums\n",
                          g_framesParsed, g_parseErrors, g_lastQty, g_lastAlarm, dbg.lastSnr,
                          g_lastFrameMs == 0 ? 0 : (millis() - g_lastFrameMs));
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20)); // drain UART well inside the radar's own report cadence
    }
}

// 3072, not the original 4096 — measured real-hardware stack high-water
// mark (2026-09-16 RAM audit, uxTaskGetStackHighWaterMark()) never dropped
// below ~1960 bytes free out of 4096, i.e. this task never actually used
// more than ~2136 bytes; 3072 keeps a comfortable ~930-byte (~30%) margin
// over that while giving ~1KB back to internal RAM, the scarcer resource on
// this board (see main_ui_demo.cpp's own [mem] tracking).
void radarTaskStart() { xTaskCreatePinnedToCore(radarTaskFn, "radarTask", 3072, NULL, 2, NULL, 0); }
