#include "GNSS.h"
#include "core/AppConfig.h"
#include "core/SharedState.h"
#include "pincfg.h"
#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <math.h>

// Confirmed on real hardware 2026-09-15 by sweeping candidate bauds and
// dumping raw bytes: this specific M10N breakout is NOT the common
// 9600-by-default kind — 9600 decoded as binary garbage (proved it wasn't
// a wiring fault), 38400 decoded clean, valid NMEA immediately (12 sats,
// real fix). Don't assume 9600 for a "default" M10N breakout again; verify
// per-board like this was.
static const uint32_t kGnssBaud = 38400;

// Spec section 5.3: raw GNSS speed must be filtered (median, then EMA/
// low-pass) before it drives the audio-gate hysteresis, so sensor noise
// can't itself cause the 60 km/h gate to chatter — the hysteresis band in
// AppConfig only protects against genuine speed oscillation, not noise on
// top of it.
class SpeedFilter {
public:
    float push(float raw) {
        hist[histIdx] = raw;
        histIdx = (histIdx + 1) % 3;
        if (histFill < 3) histFill++;

        float median = raw;
        if (histFill == 3) {
            float a = hist[0], b = hist[1], c = hist[2];
            median = fmaxf(fminf(a, b), fminf(fmaxf(a, b), c)); // median of 3
        }

        // Reads cfg directly (no lock) — same conscious simplification
        // AppConfig.h documents for SimTask: every field is an
        // independently-aligned float, so a cross-task read during a
        // Settings write sees only one field's old-or-new value, never a
        // torn one, and there's no multi-field invariant here to protect.
        float alpha = cfg.gnssSpeedFilterAlpha;
        ema = emaInit ? (alpha * median + (1.0f - alpha) * ema) : median;
        emaInit = true;
        return ema;
    }

    void reset() { histFill = 0; histIdx = 0; emaInit = false; }

private:
    float hist[3] = {0, 0, 0};
    uint8_t histIdx = 0, histFill = 0;
    float ema = 0;
    bool emaInit = false;
};

static TinyGPSPlus gps;

// Sunrise/sunset for the Dashboard clock icon (spec section 15.2: "no
// light sensor" — use GNSS date+time+lat+lon instead). Standard solar
// declination approximation (Cooper's equation) + hour-angle formula, no
// equation-of-time correction — good to within a few minutes near
// equinoxes, which is all an icon swap needs; not used for anything
// safety-related.
static bool computeDaytime(float latDeg, float lonDeg, int year, int month, int day, int utcHour, int utcMinute) {
    static const int kCumDays[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    int dayOfYear = kCumDays[month - 1] + day + ((leap && month > 2) ? 1 : 0);

    float declRad = (23.45f * (float)M_PI / 180.0f) * sinf(2.0f * (float)M_PI * (284.0f + dayOfYear) / 365.0f);
    float latRad = latDeg * (float)M_PI / 180.0f;

    float cosH = -tanf(latRad) * tanf(declRad);
    if (cosH <= -1.0f) return true;  // polar day: sun never sets
    if (cosH >= 1.0f) return false;  // polar night: sun never rises
    float hHours = acosf(cosH) * (180.0f / (float)M_PI) / 15.0f;

    float solarNoonUtc = 12.0f - lonDeg / 15.0f;
    float sunriseUtc = solarNoonUtc - hHours;
    float sunsetUtc = solarNoonUtc + hHours;
    float nowUtc = utcHour + utcMinute / 60.0f;

    return nowUtc >= sunriseUtc && nowUtc < sunsetUtc;
}

// File-scope (not function-local) so gnssMsSinceStationary() below can read
// it — see GNSS.h's comment on that function for what this tracks.
static uint32_t lastMovingMs = 0;

uint32_t gnssMsSinceStationary() { return millis() - lastMovingMs; }

static void gnssTaskFn(void *) {
    Serial2.begin(kGnssBaud, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);

    SpeedFilter speedFilter;
    esp_task_wdt_add(NULL);
    uint32_t lastFixMs = 0;
    uint32_t lastValidSentenceMs = 0;
    uint32_t lastPassedChecksum = 0;
    uint32_t lastDebugMs = 0;

    // "No fix yet" and "not receiving anything" used to both show as FAULT
    // — misleading, since the first one is the normal state while cold-
    // starting/indoors and the second is an actual wiring/power/baud
    // problem. linkAlive tracks the second, separately from fix.
    const uint32_t kLinkTimeoutMs = 5000; // generous vs. NMEA's ~1s sentence burst cadence

    for (;;) {
        while (Serial2.available()) {
            gps.encode(Serial2.read());
        }

        uint32_t passedChecksum = gps.passedChecksum();
        if (passedChecksum != lastPassedChecksum) {
            lastPassedChecksum = passedChecksum;
            lastValidSentenceMs = millis();
        }
        bool linkAlive = lastValidSentenceMs != 0 && (millis() - lastValidSentenceMs) < kLinkTimeoutMs;

        bool freshFix = gps.location.isValid() && gps.location.isUpdated();
        if (freshFix) lastFixMs = millis();
        // GNSS considered "lost" if no fresh fix for this long — spec
        // section 23: never hold/assume a stale speed once lost, disable
        // the audio gate, show fault. Tunable from Settings > Sensors.
        uint32_t fixTimeoutMs = (uint32_t)(cfg.gnssFixTimeoutS * 1000.0f);
        bool haveRecentFix = gps.location.isValid() && (millis() - lastFixMs) < fixTimeoutMs;

        GnssSnapshot snap;
        snap.fix = haveRecentFix;
        snap.linkAlive = linkAlive;
        if (haveRecentFix && gps.speed.isValid()) {
            snap.rawSpeedKmh = (float)gps.speed.kmph();
            snap.egoSpeedKmh = speedFilter.push(snap.rawSpeedKmh);
        } else {
            speedFilter.reset();
            snap.rawSpeedKmh = 0;
            snap.egoSpeedKmh = 0; // consumers must gate on .fix, never trust this while !fix
        }
        snap.satCount = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;

        // Position, independent of time/date validity — map/SpeedLimitMap.cpp
        // (added 2026-09-15) needs lat/lon whenever there's a fix at all, not
        // only once the clock's own separate fields are also valid. Moved out
        // of the timeValid block below, which used to bundle lon/lat in with
        // it purely because the Dashboard clock (the only consumer before
        // now) happened to need both; that was never a real dependency
        // between the two. No behavior change for the clock: every case
        // where timeValid was true already had location.isValid() as one of
        // its ANDed conditions, so lon/lat were always set in that case
        // either way — this only ADDS the case of "fix valid, date/time not
        // yet" where they're now populated too.
        if (haveRecentFix && gps.location.isValid()) {
            snap.lonDeg = (float)gps.location.lng();
            snap.latDeg = (float)gps.location.lat();
        }

        // Course over ground (spec architecture note: "M10N xác định vị trí,
        // tốc độ, hướng" — heading was the missing piece). A GPS module
        // derives course from the motion between fixes, not a compass, so
        // it's meaningless/noisy near-stationary — gated on a minimum speed,
        // same "don't trust a noisy low-signal reading" reasoning as the
        // speed filter's own median+EMA above. kGnssMotionThresholdKmh (see
        // GNSS.h) is this project's one shared constant for "real motion vs.
        // GPS noise floor" — also reused just below for the auto-dim gate.
        snap.headingValid = haveRecentFix && gps.course.isValid() && snap.rawSpeedKmh > kGnssMotionThresholdKmh;
        if (snap.headingValid) snap.headingDeg = (float)gps.course.deg();

        // Dashboard clock + day/night icon: only trust GPS time/date
        // alongside a real position fix, since both the local-time
        // estimate and the sunrise/sunset calc need lat/lon anyway — same
        // "don't show it if we don't trust it" rule as speed above.
        snap.timeValid = haveRecentFix && gps.time.isValid() && gps.date.isValid() && gps.location.isValid();
        if (snap.timeValid) {
            snap.utcHour = gps.time.hour();
            snap.utcMinute = gps.time.minute();
            snap.daytime = computeDaytime(snap.latDeg, snap.lonDeg, gps.date.year(), gps.date.month(),
                                           gps.date.day(), snap.utcHour, snap.utcMinute);
        }

        // Auto-dim's "vehicle stationary" gate (Dashboard.cpp's
        // burnInTimerCb) — see GNSS.h's gnssMsSinceStationary() comment.
        // !haveRecentFix counts as "moving" (reset the timer) — never assume
        // parked just because the fix dropped.
        if (!haveRecentFix || snap.egoSpeedKmh > kGnssMotionThresholdKmh) lastMovingMs = millis();

        gnssPublish(snap);

        uint32_t now = millis();
        if (now - lastDebugMs > 3000) {
            lastDebugMs = now;
            Serial.printf("[gnss] fix=%d link=%d sats=%d speed=%.1fkm/h chars=%lu sentencesWithFix=%lu "
                          "failedChecksum=%lu\n",
                          snap.fix, snap.linkAlive, snap.satCount, (double)snap.egoSpeedKmh,
                          gps.charsProcessed(), gps.sentencesWithFix(), gps.failedChecksum());
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50)); // drain UART well inside NMEA's own ~1s sentence cadence
    }
}

// 3072, not the original 4096 — same real-hardware measurement basis as
// radar/LD2451.cpp's own stack-size comment (2026-09-16 RAM audit): high-
// water mark never dropped below ~1944 bytes free of 4096, i.e. never used
// more than ~2152 bytes, leaving a comfortable ~920-byte (~30%) margin at
// 3072.
void gnssTaskStart() { xTaskCreatePinnedToCore(gnssTaskFn, "gnssTask", 3072, NULL, 2, NULL, 0); }
