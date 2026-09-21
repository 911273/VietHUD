#pragma once
#include <Arduino.h>

#define MAX_TARGETS 5

enum Relation { SAME_LANE, ADJACENT_LEFT, ADJACENT_RIGHT, OPPOSITE, UNKNOWN };

struct SimTarget {
    bool active = false;
    float distanceM = 0;
    float angleDeg = 0;
    // Independently-filtered lateral (x) offset, meters, +=right (target-
    // tracking requirement 2026-09-21, #9: "smooth distance, x and y
    // independently"). radar/LD2451.cpp's Track::lateralM is the real,
    // per-axis-filtered channel this comes from; angleDeg above is only a
    // DERIVED convenience for diagnostics/display (atan2 of the filtered
    // x/y), not itself an independently-filtered value. ui/Dashboard.cpp's
    // on-screen target placement reads this directly instead of
    // recomputing distance*sin(angle).
    float lateralM = 0;
    // Stable virtual track ID (requirement #1) — assigned once when
    // radar/LD2451.cpp's tracker first creates the underlying Track, never
    // reused even if this array SLOT later gets recycled for a different
    // physical vehicle. 0 for SimTask.cpp's demo targets (not meaningfully
    // "tracked" the same way — see that file).
    uint32_t trackId = 0;
    float closingSpeedMps = 0; // positive = approaching
    Relation relation = UNKNOWN;
    float confidence = 0;
    float ttcS = 0;
    // Raw LD2451 per-target SNR byte (0-255), diagnostic only — NOT folded
    // into confidence above (see radar/LD2451.cpp's processReportFrame():
    // no real-world good/bad SNR data exists yet to calibrate a formula
    // against, same "don't fabricate precision you don't have" reasoning as
    // the fixed confidence value). Exists so a human can watch real SNR
    // values against real targets (Settings > Radar > Link, or showAngle-
    // style Dashboard label) and build that calibration later.
    float snr = 0;

    // Tailgating flag (spec section 33's `minDistanceM`, added 2026-09-16 —
    // see core/AppConfig.h's own comment on why this needs to be distance-
    // only, independent of ttcS/closingSpeedMps above): true when this is a
    // SAME_LANE target closer than cfg.minDistanceM, regardless of whether
    // it's currently closing. ui/Dashboard.cpp and log/TripLogger.cpp both
    // read this; radar/LD2451.cpp is the only writer.
    bool tooClose = false;

    // Smoothed d(closingSpeedMps)/dt estimate, m/s^2 (radar/LD2451.cpp's
    // Track::closingAccelMps2) — diagnostic/tuning value alongside the
    // derived RadarSnapshot::harshBrakeWarning below (Settings/WebPortal
    // don't currently surface this per-target, but it's cheap to carry and
    // useful if a future diagnostics view wants it).
    float closingAccelMps2 = 0;
};

struct RadarSnapshot {
    SimTarget targets[MAX_TARGETS];
    int primaryIdx = -1;
    // Fail-safe default: false, not true. Before radar/LD2451.cpp's task has
    // ever published a real snapshot (no frame received yet this boot, or
    // the module was never plugged in at all), this compile-time default is
    // what the Dashboard's status dot actually shows — a collision-warning
    // sensor should read as disconnected/red until proven otherwise, never
    // green by default. User-reported 2026-09-16: unplugging the LD2451
    // still showed green indefinitely, traced to this defaulting true
    // combined with the offline-timeout check requiring at least one frame
    // to have ever arrived (see LD2451.cpp's radarTaskFn — that guard is
    // removed alongside this fix).
    bool online = false;
    bool audioAllowed = false; // TTC/audio-gate result — depends on both radar
                                // and ego speed, computed by the same task that
                                // has both, published alongside the targets.

    // Sudden-closing-speed ("harsh braking ahead") warning, added
    // 2026-09-16 — true if ANY confirmed SAME_LANE target's closing speed
    // is accelerating faster than cfg.harshBrakeAccelMps2 (see
    // radar/LD2451.cpp), not just the primary target: a car braking hard
    // just outside the primary-selection criteria (e.g. still UNKNOWN
    // relation for one more frame) is still worth flagging early. Feeds
    // both ui/Dashboard.cpp's own distinct visual alert (deliberately NOT
    // the TTC flash overlay — see that file's "one color = one specific
    // risk" comment) and radar.audioAllowed's gate below.
    bool harshBrakeWarning = false;

    // Live link diagnostics for Settings > Radar (radar/LD2451.cpp), same
    // purpose as GnssSnapshot's linkAlive/rawSpeedKmh below — lets a human
    // tell "genuinely offline" apart from "online but nothing detected
    // right now", and gives something concrete to watch while debugging a
    // reported fault instead of guessing.
    uint32_t framesParsed = 0;
    uint32_t parseErrors = 0;
    uint8_t lastTargetQty = 0;
    uint8_t lastAlarm = 0;
    uint8_t lastSnr = 0; // first target's raw SNR byte from the most recent non-empty frame — see SimTarget::snr
};

struct GnssSnapshot {
    float egoSpeedKmh = 50;   // filtered (median + EMA — see gnss/GNSS.cpp), what consumers should use
    float rawSpeedKmh = 0;    // unfiltered module reading, published for Settings > Sensors diagnostics only
    bool fix = true;          // a real, current position+speed fix — this is what safety logic (audio gate,
                                // TTC's ego speed) must key off, NOT linkAlive below.
    // Purely a UI/diagnostic signal — distinguishes "module is alive and
    // sending valid NMEA, just hasn't found satellites yet" (normal,
    // expected while indoors/cold-starting) from "not receiving anything at
    // all" (an actual wiring/power/baud problem). Added 2026-09-15 because
    // both cases used to show identically as "FAULT" and that's misleading
    // — see gnss/GNSS.cpp for how it's derived.
    bool linkAlive = false;
    int satCount = 0;

    // Real GNSS time/position for the Dashboard clock (spec section 15.2).
    // UTC only; local offset is estimated from longitude, see
    // gnss/GNSS.cpp / ui/Dashboard.cpp.
    bool timeValid = false;
    int utcHour = 0, utcMinute = 0;
    float lonDeg = 0;
    float latDeg = 0;
    // Sunrise/sunset estimate from GNSS date+time+position (spec section
    // 15.2: "no light sensor" — this IS that calculation). Only meaningful
    // when timeValid; defaults true (sun) so a not-yet-fixed clock icon
    // shows something neutral rather than defaulting to night.
    bool daytime = true;

    // Course over ground, degrees, 0=North/clockwise (standard NMEA COG) —
    // added 2026-09-15 for map/SpeedLimitMap.cpp's map-matching (the
    // architecture this was requested against: "M10N xác định vị trí, tốc
    // độ, hướng" — position+speed were already here, heading wasn't). Only
    // meaningful when headingValid (a GPS module reports course from
    // consecutive fixes, not a compass — unreliable/noisy near-stationary,
    // same reasoning speed itself needs the median+EMA filter in
    // gnss/GNSS.cpp). NOT YET consumed by the current nearest-point
    // matcher (see SpeedLimitMap.cpp) — that's a v1 simplification, not a
    // design decision that heading doesn't matter; captured now so a
    // direction-aware matcher can use it later without another GNSS.cpp
    // change.
    bool headingValid = false;
    float headingDeg = 0;
};

// Published by map/SpeedLimitManager.cpp — the ESP32-side "map matching"
// half of the architecture (M10N gives position+speed+heading; this
// cross-references it against a speed-limit database loaded from the
// onboard microSD, built offline on a PC by tools/map_builder/ — see
// map/SpeedMapFormat.h and map/SpeedLimitManager.h for the full picture).
// HONESTY NOTE, same as every other sensor in this project before real
// hardware existed for it: no microSD card has been tested against this
// board yet (SD_CS_PIN etc. in pincfg.h are still unconfirmed), so `valid`
// legitimately stays false until a real card with a real database is
// wired in — that's correct, honest behavior (ui/Dashboard.cpp shows "--"),
// not a bug.
struct RoadInfoSnapshot {
    bool mapLoaded = false;   // an SD card was mounted and a database was validated at boot
    bool valid = false;       // a segment WITH a known speed limit was matched to the current GNSS fix just now
    float speedLimitKmh = -1; // meaningless unless valid
    float matchDistanceM = -1; // perpendicular distance from the fix to the matched segment, diagnostic only

    // Diagnostic-only fields (Settings > Sensors > Speed Map, /api/speedlimit,
    // /api/speedmap/debug) — NOT shown on the Dashboard itself (spec section
    // 24: source/confidence/road ID belong in Settings/diagnostics, not the
    // driving screen). `source` is a SpeedSource enum value (see
    // map/SpeedMapFormat.h) kept as a raw uint8_t here rather than pulling
    // that enum into this generic pub/sub header — map/SpeedLimitManager.h's
    // speedSourceStr() converts it for display.
    uint8_t source = 0;
    float confidence = 0; // 0.0-1.0, meaningful even when !valid (e.g. a road matched but with no known speed limit)
    uint32_t roadId = 0;

    // Upcoming speed-limit-change lookahead (user-requested 2026-09-21,
    // "canh bao gioi han toc do doan duong tiep theo, bao truoc khoang
    // 100m") — map/SpeedLimitManager.cpp projects ~100m ahead along the
    // current heading and re-matches THAT point; these two are only ever
    // set true/valid when that ahead-match found a road with a speed limit
    // DIFFERENT from `speedLimitKmh` above (re-confirming the same limit
    // ahead isn't a "change" worth warning about). Shown on the Dashboard
    // as a small "upcoming limit" hint next to the current sign.
    bool aheadLimitValid = false;
    float aheadSpeedLimitKmh = -1; // meaningless unless aheadLimitValid
    float aheadDistanceM = -1;     // meaningless unless aheadLimitValid — the actual lookahead distance used
                                    // (map/SpeedLimitManager.cpp's kAheadLookaheadM), carried through rather
                                    // than hardcoded again in the UI, so the two can never drift apart

    // Upcoming speed-camera warning (feature-requested 2026-09-21, "tai du
    // lieu ve canh bao giao thong, gom camera cung nhu gioi han toc do") —
    // map/SpeedLimitManager.cpp's own separate scan over the CameraPoint[]
    // loaded from /speedmap/cameras.bin (map/SdCardManager.cpp), completely
    // independent of the road-segment matching above (a camera can be
    // "ahead" regardless of whether the current road segment itself
    // matched). Sourced from OSM's highway=speed_camera tag, NOT Waze —
    // see CameraPoint's own SpeedMapFormat.h comment for why Waze was never
    // a legally viable source for this project.
    bool cameraAheadValid = false;
    float cameraAheadDistanceM = -1;  // meaningless unless cameraAheadValid
    float cameraSpeedLimitKmh = -1;   // the camera's own tagged limit if OSM had one, else whatever road speedLimitKmh above says (may itself be -1/unknown)

    // Traffic alerts (Khu dân cư, cấm vượt, trạm thu phí, đèn tín hiệu)
    bool residentArea = false; // true if inside resident area
    bool residentAreaAheadValid = false;
    float residentAreaAheadDistM = -1;
    bool residentAreaIsStart = true; // true=R.420 (start), false=R.421 (end)

    bool noOvertaking = false; // true if inside no-overtake zone
    bool noOvertakingAheadValid = false;
    float noOvertakingAheadDistM = -1;
    bool noOvertakingIsStart = true; // true=P.125 (start), false=DP.133 (end)

    bool tollBoothAheadValid = false;
    float tollBoothAheadDistM = -1;

    bool trafficLightAheadValid = false;
    float trafficLightAheadDistM = -1;

    // General upcoming sign hint
    uint8_t nextSignType = 0;
    float nextSignDistanceM = -1;
    uint8_t nextSignSpeedLimit = 0;
};

// Mutex-protected publish/snapshot pairs (spec docs/V1.2_hardening_proposal.md
// section A.2 "TargetBuffer" pattern). The sim task — and later, Phase 2's
// real radar/GNSS UART tasks — publish; the UI task only ever reads a copy,
// never a pointer into live data, so a render can never race a mid-update
// struct.
void sharedStateInit();

void radarPublish(const RadarSnapshot &s);
RadarSnapshot radarSnapshot();

void gnssPublish(const GnssSnapshot &s);
GnssSnapshot gnssSnapshot();

void roadInfoPublish(const RoadInfoSnapshot &s);
RoadInfoSnapshot roadInfoSnapshot();
