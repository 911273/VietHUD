#pragma once
#include <Arduino.h>

struct GnssSnapshot {
    float egoSpeedKmh = 50;   // filtered (median + EMA — see gnss/GNSS.cpp), what consumers should use
    float rawSpeedKmh = 0;    // unfiltered module reading, published for Settings > Sensors diagnostics only
    bool fix = true;          // a real, current position+speed fix — this is what safety logic (speeding
                                // overlay, GNSS status) must key off, NOT linkAlive below.
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
    // added 2026-09-15 for map/SpeedLimitManager.cpp's map-matching (the
    // architecture this was requested against: "M10N xác định vị trí, tốc
    // độ, hướng" — position+speed were already here, heading wasn't). Only
    // meaningful when headingValid (a GPS module reports course from
    // consecutive fixes, not a compass — unreliable/noisy near-stationary,
    // same reasoning speed itself needs the median+EMA filter in
    // gnss/GNSS.cpp).
    bool headingValid = false;
    float headingDeg = 0;
};

// Published by map/SpeedLimitManager.cpp — the ESP32-side "map matching"
// half of the architecture (M10N gives position+speed+heading; this
// cross-references it against a speed-limit database loaded from the
// onboard microSD, built offline on a PC by tools/map_builder/ — see
// map/SpeedMapFormat.h and map/SpeedLimitManager.h for the full picture).
// HONESTY NOTE, same as every other sensor in this project before real
// hardware existed for it: `valid` legitimately stays false until a real
// card with a real database is wired in and a GNSS fix is matched against
// it — that's correct, honest behavior (ui/Dashboard.cpp shows "--"), not a
// bug.
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
    // matched). Sourced from OSM's highway=speed_camera tag / the WYN sign
    // CSV's sign_type=4 rows — see CameraPoint's own SpeedMapFormat.h
    // comment.
    bool cameraAheadValid = false;
    float cameraAheadDistanceM = -1;  // meaningless unless cameraAheadValid
    float cameraSpeedLimitKmh = -1;   // the camera's own tagged limit if OSM/WYN had one, else whatever road speedLimitKmh above says (may itself be -1/unknown)

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
// section A.2 "TargetBuffer" pattern). gnss/GNSS.cpp and
// map/SpeedLimitManager.cpp publish; the UI task only ever reads a copy,
// never a pointer into live data, so a render can never race a mid-update
// struct.
void sharedStateInit();

void gnssPublish(const GnssSnapshot &s);
GnssSnapshot gnssSnapshot();

void roadInfoPublish(const RoadInfoSnapshot &s);
RoadInfoSnapshot roadInfoSnapshot();
