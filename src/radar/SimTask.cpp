#include "SimTask.h"
#include "core/SharedState.h"
#include "core/AppConfig.h"
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>

// Per-track synthetic motion so targets glide smoothly instead of jumping
// (spec section 9/28: "khong de target nhay manh giua cac frame").
struct TrackDef {
    Relation relation;
    float distBase, distAmp, distPeriodMs, distPhase;
    float angleDeg;
    float closingBase, closingAmp, closingPeriodMs, closingPhase;
};

static const TrackDef kTracks[MAX_TARGETS] = {
    {SAME_LANE,      35, 25, 9000,  0.0f,   0.0f,  3, 6, 7000, 0.0f},
    {ADJACENT_LEFT,  30, 15, 11000, 1.0f,  -9.0f,  0, 3, 8000, 1.0f},
    {ADJACENT_RIGHT, 40, 18, 12500, 2.0f,   9.0f,  0, 3, 8500, 2.0f},
    {OPPOSITE,       60, 40, 6000,  0.5f,  -3.0f, -18, 6, 6000, 0.5f},
    {UNKNOWN,        50, 20, 15000, 1.5f,   6.0f,  1, 4, 9000, 1.5f},
};

static void tick(uint32_t nowMs) {
    RadarSnapshot radar;
    // GNSS is real now (gnss/GNSS.cpp, its own task) — this tick only fakes
    // radar. It still needs SOME ego speed to drive TTC/audio-gate math
    // below, so it reads the real published snapshot rather than keeping
    // its own fake one; see the loop below.
    GnssSnapshot gnss = gnssSnapshot();

    // Radar fail-safe window (spec T14 — no fake targets while "lost")
    radar.online = fmodf(nowMs, 25000.0f) > 2000.0f;

    // Active target count cycles 0..maxTargets..0 (spec section 29: must
    // show the real count, never a fixed number), capped by cfg.maxTargets.
    int cycleCount = (int)roundf(2.5f + 2.5f * sinf((nowMs / 10000.0f) * 2.0f * (float)M_PI));
    cycleCount = constrain(cycleCount, 0, (int)cfg.maxTargets);

    radar.primaryIdx = -1;
    float bestDist = 1e9f; // closest SAME_LANE target wins — see the selection check below

    for (int i = 0; i < MAX_TARGETS; i++) {
        SimTarget &t = radar.targets[i];
        if (!radar.online || i >= cycleCount) {
            t.active = false;
            continue;
        }
        const TrackDef &tr = kTracks[i];
        float distanceM = tr.distBase + tr.distAmp * sinf((nowMs / tr.distPeriodMs + tr.distPhase) * 2.0f * (float)M_PI);
        if (distanceM < 3.0f) distanceM = 3.0f;
        float closingSpeedMps =
            tr.closingBase + tr.closingAmp * sinf((nowMs / tr.closingPeriodMs + tr.closingPhase) * 2.0f * (float)M_PI);

        // Radar min-target-speed filter (spec section 24) — targets moving
        // slower than the configured threshold are not reported.
        if (fabsf(closingSpeedMps) * 3.6f < cfg.minTargetSpeedKmh) {
            t.active = false;
            continue;
        }

        t.active = true;
        t.distanceM = distanceM;
        t.angleDeg = tr.angleDeg;
        // Real radar/LD2451.cpp now exposes this as its own independently-
        // filtered channel (2026-09-21 target-tracking rework) and
        // ui/Dashboard.cpp reads it directly for on-screen placement —
        // populated here too so the demo preview positions its fake
        // targets the same way instead of defaulting to 0 (dead center).
        t.lateralM = distanceM * sinf(tr.angleDeg * (float)M_PI / 180.0f);
        t.closingSpeedMps = closingSpeedMps;
        t.relation = tr.relation;
        t.confidence = 0.85f;
        t.ttcS = (t.closingSpeedMps <= 0.05f) ? INFINITY : (t.distanceM / t.closingSpeedMps);

        // Closest same-lane target wins, not "closing fastest" — matches
        // the real radar/LD2451.cpp's own 2026-09-21 fix (see its comment)
        // so demo mode previews the same always-shows-distance behavior.
        if (t.relation == SAME_LANE && t.confidence >= cfg.minConfidence && t.distanceM < bestDist) {
            bestDist = t.distanceM;
            radar.primaryIdx = i;
        }
    }

    // Audio gate with hysteresis (spec section 13)
    float audioDisableKmh = cfg.audioEnableKmh - cfg.hysteresisKmh;
    static bool audioSpeedEnabled = false;
    if (gnss.egoSpeedKmh > cfg.audioEnableKmh) audioSpeedEnabled = true;
    else if (gnss.egoSpeedKmh < audioDisableKmh) audioSpeedEnabled = false;

    radar.audioAllowed = cfg.audioEnabled && gnss.fix && audioSpeedEnabled && radar.primaryIdx >= 0 &&
                          radar.targets[radar.primaryIdx].ttcS < cfg.ttcWarnS;

    radarPublish(radar);
}

static void simTaskFn(void *) {
    esp_task_wdt_add(NULL);
    for (;;) {
        tick(millis());
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100)); // sim tick rate, independent of the UI's own refresh rate
    }
}

void simTaskStart() { xTaskCreatePinnedToCore(simTaskFn, "simTask", 4096, NULL, 2, NULL, 0); }
