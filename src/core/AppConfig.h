#pragma once
#include <Arduino.h>
#include <math.h> // isfinite() — see sanitizeConfig() below

// User-tunable settings, persisted to NVS (core/NvsStore.cpp) and edited
// from ui/Settings.cpp and net/WebPortal.cpp's /config page. Trimmed
// 2026-09-21 to VietHUD's actual scope (GPS-only offline speed-limit/
// camera/sign warnings) when radar was removed entirely — every field that
// only ever fed the HLK-LD2451/target tracking/TTC risk engine (max range,
// target count, TTC thresholds, audio-gate hysteresis, lane width, tracking
// smoothing, per-target display toggles, demo mode) is gone with it rather
// than left as dead config nobody reads; see git history if any of that is
// ever needed again.
struct AppConfig {
    float brightness = 100; // % — drives the real backlight PWM
    // Gates the vehicle being continuously STATIONARY (see
    // ui/Dashboard.cpp's burn-in-mitigation header comment and
    // gnss/GNSS.h's gnssMsSinceStationary()). 0 = off.
    float autoDimMin = 3;

    // Master alert-audio toggle (tone chimes AND voice cues, see
    // audio/AudioPlayer.h) — user can mute camera/sign/speed-limit warnings
    // without touching device volume. Was originally the radar audio-gate's
    // own on/off switch; repurposed 2026-09-21 to the same role for the
    // sign/camera alerts that replaced radar, no behavior change to the
    // switch itself (still a plain on/off).
    bool audioEnabled = true;

    // Real GNSS calibration (gnss/GNSS.cpp reads these directly every tick —
    // cross-task read without a lock is safe here: a single float/bool field
    // has no torn/garbage intermediate value to race on). spec section
    // 5.3/16.6: speed filter and fix-loss handling must be tunable, not
    // hardcoded, once real hardware exists to tune against.
    float gnssSpeedFilterAlpha = 0.30f; // EMA weight on the new sample; higher = less smoothing, more lag
    float gnssFixTimeoutS = 3.0f;       // no fresh fix for this long -> report GNSS lost (spec T13)

    // Trip logging (log/TripLogger.cpp) — periodic + event-triggered CSV log
    // to the microSD card, for reviewing a drive's speed-limit/camera/sign
    // warnings afterward instead of only road-testing by feel. Defaults ON:
    // logging is a passive, fail-open diagnostic (no effect on safety logic
    // either way), but this switch exists for anyone who'd rather not
    // accumulate files on their card.
    bool tripLoggingEnabled = true;

    // Local WiFi AP credentials (net/WebPortal.cpp) — plain char arrays, not
    // floats, so sanitizeConfig()/clampConfig() below don't touch them (both
    // only cover numeric fields — see their own "named field-by-field"
    // comments). Only the credentials persist; WiFi's on/off state does NOT
    // — it always starts OFF at boot regardless of what it was last session
    // (user-requested 2026-09-14: "Mặc định là wifi tắt"), toggled at
    // runtime only, via a 3s+ hold on the Dashboard or the switch in
    // Settings > WiFi — see net/WebPortal.h. Defaults renamed 2026-09-21
    // (radar_car -> VietHUD product rename) — no functional change.
    char wifiSsid[32] = "VietHUD";
    char wifiPassword[64] = "viethud123"; // WPA2 needs >=8 chars — see WebPortal.cpp's applyWifiState() fallback

    // Display settings (user-requested 2026-09-15). Both are floats used as
    // small enums — no dropdown widget exists in Settings.cpp, only sliders.
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
};

// Single shared instance, defined in main_ui_demo.cpp. NOTE: read without a
// lock from other tasks — a conscious, scoped simplification, not the full
// mutex-protected ConfigStore design in docs/V1.2_hardening_proposal.md
// section A.2. Every field is an independently-aligned float/bool/char[], so
// a cross-task read during a Settings write can only ever see one field's
// old-or-new value, never a torn/garbage one — there is no multi-field
// invariant here that a reader depends on being atomic.
extern AppConfig cfg;

inline void clampConfig(AppConfig &c) {
    c.brightness = constrain(c.brightness, 5.0f, 100.0f);
    c.autoDimMin = constrain(c.autoDimMin, 0.0f, 30.0f);
    c.gnssSpeedFilterAlpha = constrain(c.gnssSpeedFilterAlpha, 0.05f, 0.90f);
    c.gnssFixTimeoutS = constrain(c.gnssFixTimeoutS, 1.0f, 10.0f);
    c.screenRotation = constrain(c.screenRotation, 0.0f, 3.0f);
    c.themeMode = constrain(c.themeMode, 0.0f, 2.0f);
}

// Replaces any non-finite (NaN/Inf) field with AppConfig's own default —
// guards against a corrupted NVS flash page (bit rot, power loss mid-write)
// producing a garbage float bit pattern. Deliberately checked BEFORE
// clampConfig(): constrain(NaN, lo, hi) returns NaN (every comparison
// against NaN is false), so clamping alone can't catch this. Named
// field-by-field (not a generic byte/float scan over the whole struct)
// because AppConfig mixes float/bool/char[] fields — bools/strings can't be
// NaN, and a blind reinterpret_cast<float*> scan would misread their bytes
// and misalign every float field after them.
inline void sanitizeConfig(AppConfig &c) {
    static const AppConfig d;
    if (!isfinite(c.brightness)) c.brightness = d.brightness;
    if (!isfinite(c.autoDimMin)) c.autoDimMin = d.autoDimMin;
    if (!isfinite(c.gnssSpeedFilterAlpha)) c.gnssSpeedFilterAlpha = d.gnssSpeedFilterAlpha;
    if (!isfinite(c.gnssFixTimeoutS)) c.gnssFixTimeoutS = d.gnssFixTimeoutS;
    if (!isfinite(c.screenRotation)) c.screenRotation = d.screenRotation;
    if (!isfinite(c.themeMode)) c.themeMode = d.themeMode;
}
