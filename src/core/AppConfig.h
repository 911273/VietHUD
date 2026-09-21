#pragma once
#include <Arduino.h>
#include <math.h> // isfinite() — see sanitizeConfig() below

// spec section 33 — trimmed to fields that have a real, observable effect in
// this sensor-less demo (see docs/radar_car_V1.1_spec.md and
// docs/V1.2_hardening_proposal.md for the fields not yet included: radar
// direction/SNR/trigger-count, lane-model settings — no lane-corridor model
// exists yet).
struct AppConfig {
    float maxRangeM = 100;
    // Pushed straight to the LD2451 itself (radar/LD2451.cpp's
    // configureRadar(), command 0x0002) — the MODULE's own onboard filter,
    // not a client-side re-check in this firmware. Default lowered 5->1
    // (2026-09-21, user-reported "neu xe khong di chuyen thi van hien thi
    // khoang cach voi xe phia truoc"): at 5, a target whose RELATIVE
    // closing speed (not either vehicle's absolute speed) falls below this
    // never gets reported by the module AT ALL — which is exactly what
    // happens when ego is also stopped/creeping behind a stopped car ahead
    // (relative speed ~0), silently blanking the distance readout in
    // precisely the stop-and-go-traffic case core/AppConfig.h's
    // minDistanceM/tooClose comment already says must still be caught.
    // 1 (not 0) keeps a small floor against pure sensor noise oscillating
    // right at zero; while actually driving, every stationary roadside
    // object already has a relative speed close to the ego's OWN speed
    // (well above any low threshold), so this doesn't meaningfully change
    // real-driving clutter filtering — classifyRelation()'s lateral-offset
    // SAME_LANE check is what actually keeps roadside clutter out, not this.
    float minTargetSpeedKmh = 1;
    float maxTargets = 5;

    float audioEnableKmh = 62;
    float hysteresisKmh = 4; // disable threshold = audioEnableKmh - hysteresisKmh
    float ttcWarnS = 3.0f;
    float ttcCritS = 1.5f;
    float minConfidence = 0.70f;

    float brightness = 100; // % — drives the real backlight PWM
    // Default 5->3 (2026-09-16, user-requested) — now gates on the vehicle
    // being continuously STATIONARY, not touch-idle time; see
    // ui/Dashboard.cpp's burn-in-mitigation header comment and
    // gnss/GNSS.h's gnssMsSinceStationary(). 0 = off.
    float autoDimMin = 3;

    bool showId = true;
    bool showSpeed = false;
    bool showTtc = false;
    bool showAngle = false;
    bool audioEnabled = true;

    // Real GNSS calibration (gnss/GNSS.cpp reads these directly every tick —
    // see the note above on why a cross-task read without a lock is fine
    // here). spec section 5.3/16.6: speed filter and fix-loss handling must
    // be tunable, not hardcoded, once real hardware exists to tune against.
    float gnssSpeedFilterAlpha = 0.30f; // EMA weight on the new sample; higher = less smoothing, more lag
    float gnssFixTimeoutS = 3.0f;       // no fresh fix for this long -> report GNSS lost (spec T13)

    // Real radar (LD2451) calibration — added 2026-09-16 once real hardware
    // existed to tune against (radar/LD2451.cpp's configureRadar() pushes
    // these to the module at boot; see its datasheet-sourced command
    // encoding). Values match the LD2451 protocol's own byte encoding
    // directly (spec section 16.1's "Radar Settings" list).
    float radarDirection = 2;      // 0=away only, 1=approach only, 2=all — LD2451 command 0x0002 byte 2
    float radarSnrLevel = 0;       // 0=module default(~4), 3-8=configurable threshold, higher=less sensitive — command 0x0003
    float radarTriggerCount = 1;   // 1-10 consecutive detections required before alarm — command 0x0003
    float radarNoTargetDelayS = 2; // seconds before "target gone" after last detection — command 0x0002

    // Physical mounting correction (user-requested 2026-09-21, "rada do lap
    // dat co the xoay cac chieu khac nhau, de xuat phuong an de chinh chieu
    // lap dat"). Confirmed with the user first: the LD2451's angle-of-
    // arrival measurement is inherently a HORIZONTAL-plane reading (its
    // antenna array only ever distinguishes left/right, never up/down), so
    // the only physically real mounting variance is a 180-degree flip
    // (connector/cable facing the opposite way, module rotated end-to-end
    // in that same horizontal plane) — NOT a 90/270-degree rotation like
    // screenRotation above, which would mean the antenna's measurement
    // plane itself turned vertical and no software transform can recover a
    // left/right reading that was never actually captured. A plain bool
    // (not a float-enum): only 2 real states exist — same "genuinely
    // binary -> bool + switch" reasoning as simpleUiMode's own comment.
    // radar/LD2451.cpp negates the raw angle reading at decode time when
    // this is true — the one place every downstream consumer (tracking,
    // lane classification, Dashboard target placement) inherits the
    // correction from.
    bool radarMountFlipped = false;

    // Multi-target persistence/smoothing tracker (radar/LD2451.cpp's
    // updateTracks(), added 2026-09-15 — spec section 9.1/9.2). The LD2451
    // has no persistent target ID and reports raw distance/speed in whole
    // meters/km-h, so consuming its frames directly (as this project did
    // before the tracker existed) meant the Dashboard's target dot/labels
    // jittered a full raw unit every single frame and could "teleport"
    // between two different physical vehicles whenever the module happened
    // to list them in a different order frame-to-frame. These three values
    // are spec 9.1's explicitly-required tunables ("Các giá trị này phải là
    // configurable/tunable").
    float radarTrackConfirmFrames = 2; // consecutive matched frames before a new target is shown (spec: 2-3)
    float radarTrackLossS = 0.5f;      // no match for this long -> drop the target (spec: ~500ms)
    float radarTrackFilterAlpha = 0.35f; // EMA weight on each frame's new distance/angle/speed reading for an
                                          // already-confirmed target; higher = less smoothing, less lag

    // Lane classification (radar/LD2451.cpp's classifyRelation(), added
    // 2026-09-14 replacing a fixed +/-10 degree angle cone). The old cone
    // was wrong by construction — a fixed ANGLE threshold means the implied
    // lane gets wider the farther away a target is (17m wide at 100m range,
    // under 1m at 5m range), which doesn't match a real lane. This is a
    // proper lateral-offset check instead: lateral = distanceM *
    // sin(angleDeg), compared against half a real lane's width, so the
    // implied lane stays a constant physical width at any range. Default
    // 1.75m = half of a standard ~3.5m lane. Angle SIGN convention (which
    // side is physically left/right) is still unverified against real
    // hardware — see the classifyRelation() comment; this only fixes how
    // far off-axis counts as "still my lane", not which side is which.
    float laneHalfWidthM = 1.75f;

    // Tailgating gate (radar/LD2451.cpp's processReportFrame(), added
    // 2026-09-16) — spec section 33 already lists `minDistanceM` as a
    // required tunable, but no code ever read it: TTC alone reports
    // TTC=INFINITY (spec section 5.2's own explicit rule) for a SAME_LANE
    // target that isn't closing, which is exactly the stop-and-go-traffic
    // case where a fixed following distance still matters even at
    // zero/negative relative speed. This is a distance-only check,
    // independent of closing speed/TTC, so it catches that gap. Spec
    // section 34's own suggested default (3m) is a hard minimum, not a
    // comfortable following distance — kept as the default since no road
    // test has tuned a better value yet.
    float minDistanceM = 3.0f;

    // Harsh-braking-ahead gate (radar/LD2451.cpp's Track::closingAccelMps2,
    // added 2026-09-16) — a SAME_LANE target's closing speed increasing
    // faster than this (m/s per second) is flagged as a sudden-braking
    // warning, independent of the current TTC value: TTC alone only reacts
    // once the gap is ALREADY dangerously short, whereas a sudden jump in
    // closing rate is itself informative (the vehicle ahead just braked, or
    // this one is closing unusually fast) before TTC necessarily crosses
    // ttcWarnS. ~3.0 m/s^2 is a rough "sudden" threshold (a hard car brake
    // is commonly ~4-6 m/s^2 deceleration) — not tuned against a real drive
    // yet, hence tunable rather than hardcoded.
    float harshBrakeAccelMps2 = 3.0f;

    // Trip logging (log/TripLogger.cpp, added 2026-09-16) — periodic +
    // event-triggered CSV log to the microSD card (docs/V1.2_hardening_proposal.md
    // section G's "SD/LittleFS logging" row), for reviewing/tuning TTC and
    // distance thresholds against a real drive afterward instead of only
    // road-testing by feel. Defaults ON: logging is a passive, fail-open
    // diagnostic (no effect on safety logic either way), but this switch
    // exists for anyone who'd rather not accumulate files on their card.
    bool tripLoggingEnabled = true;

    // Local WiFi AP credentials (net/WebPortal.cpp) — plain char arrays, not
    // floats, so sanitizeConfig()/clampConfig() below don't touch them (both
    // only cover numeric fields — see their own "named field-by-field"
    // comments). Only the credentials persist; WiFi's on/off state does NOT
    // — it always starts OFF at boot regardless of what it was last session
    // (user-requested 2026-09-14: "Mặc định là wifi tắt"), toggled at
    // runtime only, via a 3s+ hold on the Dashboard or the switch in
    // Settings > WiFi — see net/WebPortal.h.
    char wifiSsid[32] = "RadarCar";
    char wifiPassword[64] = "radarcar123"; // WPA2 needs >=8 chars — see WebPortal.cpp's applyWifiState() fallback

    // Display settings (user-requested 2026-09-15). Both are floats used as
    // small enums — no dropdown widget exists in Settings.cpp, only sliders,
    // same convention radarDirection above already establishes for this
    // codebase.
    //
    // screenRotation matches Arduino_GFX's own 0-3 rotation values exactly
    // (display/DisplayDriver.cpp passes this straight through to
    // Arduino_Canvas, and touch/TouchTask.cpp to AXS15231BTouch — both use
    // the identical convention, confirmed by reading each driver's rotation
    // switch). Default 1 = this project's boot rotation ever since
    // dispcfg.h's DEMO_ROTATION was introduced — changing this does NOT
    // take effect live (see ui/Settings.cpp's rotation row): both Dashboard
    // and Settings are laid out once at boot for whichever orientation is
    // active then, so a change only applies on the next restart.
    float screenRotation = 1;
    // themeMode: 0=Auto (today's only behavior — ui/Dashboard.cpp's
    // applyTheme() follows gnss.daytime's real sunrise/sunset calculation),
    // 1=Light, 2=Dark (both override gnss.daytime rather than replacing the
    // calculation — GNSS.cpp's sunrise/sunset math is untouched and still
    // runs regardless, since the sun icon and the local-time-from-longitude
    // estimate both still need it). Applies live, no restart needed.
    float themeMode = 0;
    // simpleUiMode (user-requested 2026-09-16, "giao dien don gian ... tang
    // tap trung ... khong can hien thi vi tri xe, chi hien thi dang so
    // khoang cach"): a plain bool (not a float-enum like themeMode/
    // screenRotation above) since it's genuinely binary — same convention
    // showId/audioEnabled/demoMode below already use for on/off toggles,
    // and it lets Settings bind it with the cheaper addSwitchRow (26px)
    // instead of addChoiceRow (43px), which mattered here: the Display
    // tab's fixed 260px non-scrolling row budget (see buildSettingsScreen's
    // own comment) had no room left for a 3rd choice row after Theme+
    // Rotation. false=Full (today's only layout — roadArea's lane lines +
    // moving target icons/dots), true=Simple — hides that graphical road
    // view entirely and shows ONLY radar.primaryIdx's distance as one large
    // number (reusing primaryDistLabel, already fed by the real same-lane
    // primary-target logic — no new radar/tracking logic needed, this is a
    // display-only declutter). Speed/limit/TTC/warning cells are untouched
    // either way — the ask was specifically to drop the car-position
    // graphic, not the other readouts. Applies live (ui/Dashboard.cpp's
    // refreshDashboard() just toggles roadArea's visibility and
    // primaryDistLabel's font/position on a mode CHANGE), no restart
    // needed — unlike screenRotation/demoMode below, nothing here resizes
    // the display or swaps which sensor task runs.
    bool simpleUiMode = false;

    // Demo mode (user-requested 2026-09-16, "them 1 nut bat tat thu
    // nghiem trong menu") — swaps the real HLK-LD2451 radar task for
    // radar/SimTask.cpp's simulated moving targets, and widens
    // map/SpeedLimitManager.cpp's match radius so the speed-limit sign can
    // be previewed on a real GNSS fix that isn't actually near a mapped
    // road (e.g. testing indoors). Applied at boot only (main_ui_demo.cpp's
    // setup() picks simTaskStart() vs. radarTaskStart() once) — like
    // screenRotation above, restart to take effect, not live. Defaults OFF:
    // this must never silently be on for real driving use, the whole point
    // is faked target data.
    bool demoMode = false;
};

// Single shared instance, defined in main_ui_demo.cpp. NOTE: read without a
// lock from SimTask (a separate FreeRTOS task/core) — a conscious, scoped
// simplification for this sim-data demo, not the full mutex-protected
// ConfigStore design in docs/V1.2_hardening_proposal.md section A.2. Every
// field is an independently-aligned float/bool, so a cross-task read during
// a Settings write can only ever see one field's old-or-new value, never a
// torn/garbage one — there is no multi-field invariant here that a reader
// depends on being atomic (unlike, say, a real safety engine's threshold
// pair). Revisit with a real ConfigStore before any real safety enforcement
// (Phase 6+) or before Settings' own slider-binding is restructured.
extern AppConfig cfg;

inline void clampConfig(AppConfig &c) {
    c.maxRangeM = constrain(c.maxRangeM, 10.0f, 100.0f);
    c.minTargetSpeedKmh = constrain(c.minTargetSpeedKmh, 0.0f, 30.0f);
    c.maxTargets = constrain(c.maxTargets, 1.0f, 5.0f);
    c.audioEnableKmh = constrain(c.audioEnableKmh, 40.0f, 120.0f);
    c.hysteresisKmh = constrain(c.hysteresisKmh, 1.0f, 10.0f);
    c.ttcWarnS = constrain(c.ttcWarnS, 1.0f, 10.0f);
    c.ttcCritS = constrain(c.ttcCritS, 0.5f, 5.0f);
    if (c.ttcCritS >= c.ttcWarnS) c.ttcCritS = c.ttcWarnS - 0.1f; // spec section 53 rule
    c.minConfidence = constrain(c.minConfidence, 0.0f, 1.0f);
    c.brightness = constrain(c.brightness, 5.0f, 100.0f);
    c.autoDimMin = constrain(c.autoDimMin, 0.0f, 30.0f);
    c.gnssSpeedFilterAlpha = constrain(c.gnssSpeedFilterAlpha, 0.05f, 0.90f);
    c.gnssFixTimeoutS = constrain(c.gnssFixTimeoutS, 1.0f, 10.0f);
    c.radarDirection = constrain(c.radarDirection, 0.0f, 2.0f);
    c.radarSnrLevel = constrain(c.radarSnrLevel, 0.0f, 8.0f);
    c.radarTriggerCount = constrain(c.radarTriggerCount, 1.0f, 10.0f);
    c.radarNoTargetDelayS = constrain(c.radarNoTargetDelayS, 0.0f, 30.0f); // LD2451 byte allows 0-255s; 30s covers any sane real use
    c.radarTrackConfirmFrames = constrain(c.radarTrackConfirmFrames, 2.0f, 3.0f); // spec 9.1's own stated range
    c.radarTrackLossS = constrain(c.radarTrackLossS, 0.2f, 2.0f);
    c.radarTrackFilterAlpha = constrain(c.radarTrackFilterAlpha, 0.10f, 0.90f);
    c.laneHalfWidthM = constrain(c.laneHalfWidthM, 0.5f, 5.0f);
    c.minDistanceM = constrain(c.minDistanceM, 1.0f, 20.0f);
    c.harshBrakeAccelMps2 = constrain(c.harshBrakeAccelMps2, 1.0f, 10.0f);
    c.screenRotation = constrain(c.screenRotation, 0.0f, 3.0f); // same "slider with divisor=1.0f sends whole numbers" convention as radarDirection above
    c.themeMode = constrain(c.themeMode, 0.0f, 2.0f);
}

// Replaces any non-finite (NaN/Inf) field with AppConfig's own default —
// guards against a corrupted NVS flash page (bit rot, power loss mid-write)
// producing a garbage float bit pattern. Deliberately checked BEFORE
// clampConfig(): constrain(NaN, lo, hi) returns NaN (every comparison
// against NaN is false), so clamping alone can't catch this — a single
// corrupted field would otherwise silently propagate NaN into TTC math,
// the audio gate, and UI labels. Added 2026-09-14 as part of the reliability
// pass. Named field-by-field (not a generic byte/float scan over the whole
// struct) because AppConfig mixes float and bool fields — bools can't be
// NaN, and a blind reinterpret_cast<float*> scan would misread their bytes
// and misalign every float field after them.
inline void sanitizeConfig(AppConfig &c) {
    static const AppConfig d;
    if (!isfinite(c.maxRangeM)) c.maxRangeM = d.maxRangeM;
    if (!isfinite(c.minTargetSpeedKmh)) c.minTargetSpeedKmh = d.minTargetSpeedKmh;
    if (!isfinite(c.maxTargets)) c.maxTargets = d.maxTargets;
    if (!isfinite(c.audioEnableKmh)) c.audioEnableKmh = d.audioEnableKmh;
    if (!isfinite(c.hysteresisKmh)) c.hysteresisKmh = d.hysteresisKmh;
    if (!isfinite(c.ttcWarnS)) c.ttcWarnS = d.ttcWarnS;
    if (!isfinite(c.ttcCritS)) c.ttcCritS = d.ttcCritS;
    if (!isfinite(c.minConfidence)) c.minConfidence = d.minConfidence;
    if (!isfinite(c.brightness)) c.brightness = d.brightness;
    if (!isfinite(c.autoDimMin)) c.autoDimMin = d.autoDimMin;
    if (!isfinite(c.gnssSpeedFilterAlpha)) c.gnssSpeedFilterAlpha = d.gnssSpeedFilterAlpha;
    if (!isfinite(c.gnssFixTimeoutS)) c.gnssFixTimeoutS = d.gnssFixTimeoutS;
    if (!isfinite(c.radarDirection)) c.radarDirection = d.radarDirection;
    if (!isfinite(c.radarSnrLevel)) c.radarSnrLevel = d.radarSnrLevel;
    if (!isfinite(c.radarTriggerCount)) c.radarTriggerCount = d.radarTriggerCount;
    if (!isfinite(c.radarNoTargetDelayS)) c.radarNoTargetDelayS = d.radarNoTargetDelayS;
    if (!isfinite(c.radarTrackConfirmFrames)) c.radarTrackConfirmFrames = d.radarTrackConfirmFrames;
    if (!isfinite(c.radarTrackLossS)) c.radarTrackLossS = d.radarTrackLossS;
    if (!isfinite(c.radarTrackFilterAlpha)) c.radarTrackFilterAlpha = d.radarTrackFilterAlpha;
    if (!isfinite(c.laneHalfWidthM)) c.laneHalfWidthM = d.laneHalfWidthM;
    if (!isfinite(c.minDistanceM)) c.minDistanceM = d.minDistanceM;
    if (!isfinite(c.harshBrakeAccelMps2)) c.harshBrakeAccelMps2 = d.harshBrakeAccelMps2;
    if (!isfinite(c.screenRotation)) c.screenRotation = d.screenRotation;
    if (!isfinite(c.themeMode)) c.themeMode = d.themeMode;
}
