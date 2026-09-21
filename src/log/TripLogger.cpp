#include "TripLogger.h"
#include "core/AppConfig.h"
#include "core/SharedState.h"
#include "map/SdCardManager.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h> // esp_reset_reason() — see tripLogResetReasonStr()
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h>

// One incrementing counter in its own NVS namespace, separate from
// AppConfig/"radarcar" (core/NvsStore.cpp) — this is boot-session
// bookkeeping, not a user setting, and doesn't belong in that file's
// load-on-boot/save-on-Apply flow. Wraps naturally at UINT32_MAX, which at
// one increment per boot is not a practical concern.
static uint32_t nextSessionId() {
    Preferences p;
    p.begin("triplog", false);
    uint32_t id = p.getUInt("session", 0) + 1;
    p.putUInt("session", id);
    p.end();
    return id;
}

const char *tripLogResetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software (esp_restart)";
        case ESP_RST_PANIC: return "panic/exception";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "SDIO";
        default: return "unknown/deep-sleep/other";
    }
}

static const char *riskLabelFor(float ttcS, float ttcWarnS, float ttcCritS) {
    if (isinf(ttcS)) return "SAFE";
    if (ttcS <= ttcCritS) return "CRITICAL";
    if (ttcS <= ttcWarnS) return "WARNING";
    return "SAFE";
}

static void tripLoggerTaskFn(void *) {
    // map/SpeedLimitManager.cpp's own task mounts the SD card at boot; this
    // just needs SD_MMC itself up (map/SdCardManager.cpp's
    // ensureSdMmcBegun(), shared and idempotent either task can trigger
    // first), so no fixed head-start delay is load-bearing here — this
    // pause is purely to let boot-time Serial logging from other tasks
    // settle first, not a correctness requirement.
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint32_t sessionId = nextSessionId();
    char logPath[48];
    snprintf(logPath, sizeof(logPath), "/triplog/session_%04lu.csv", (unsigned long)sessionId);
    Serial.printf("[triplog] session %lu -> %s\n", (unsigned long)sessionId, logPath);

    bool headerWritten = false;
    bool wasCritical = false, wasTooClose = false, wasHarshBrake = false;
    uint32_t lastSampleMs = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!cfg.tripLoggingEnabled) continue;

        if (!headerWritten) {
            // Retried every tick until it succeeds (e.g. card not mounted
            // yet at boot) rather than giving up — cheap, and losing the
            // header line forever would make the CSV harder to parse later.
            if (sdMgrAppendLine(logPath,
                                 "tMs,egoKmh,activeTargets,primDistM,primTtcS,primSnr,risk,tooClose,harshBrake,event")) {
                headerWritten = true;
                // Why the PREVIOUS run ended, written into this run's file
                // (added 2026-09-21 after analysing a real road test whose
                // ~18 minutes came back split across FIVE session files —
                // i.e. the device reset at least four times mid-drive, with
                // nothing in the logs to say why). esp_reset_reason() is
                // already read and printed at boot by main_ui_demo.cpp, but
                // Serial isn't attached in the car; persisting it here is
                // what makes a mid-drive reset diagnosable after the fact.
                char reasonLine[64];
                snprintf(reasonLine, sizeof(reasonLine), "# boot, previous run ended: %s",
                          tripLogResetReasonStr());
                sdMgrAppendLine(logPath, reasonLine);
            } else {
                continue;
            }
        }

        RadarSnapshot radar = radarSnapshot();
        GnssSnapshot gnss = gnssSnapshot();

        int activeTargets = 0;
        for (int i = 0; i < MAX_TARGETS; i++)
            if (radar.targets[i].active) activeTargets++;

        bool criticalNow =
            radar.online && radar.primaryIdx >= 0 && radar.targets[radar.primaryIdx].ttcS <= cfg.ttcCritS;
        bool tooCloseNow = radar.online && radar.primaryIdx >= 0 && radar.targets[radar.primaryIdx].tooClose;
        bool harshBrakeNow = radar.harshBrakeWarning;

        // Rising-edge only (not "while true") — one EVENT row marking the
        // moment a condition STARTS is what's worth finding again later; a
        // row every 200ms for the whole duration it stays true would just
        // bloat the file with the same fact repeated.
        bool isEvent =
            (criticalNow && !wasCritical) || (tooCloseNow && !wasTooClose) || (harshBrakeNow && !wasHarshBrake);
        uint32_t now = millis();
        bool isSample = (now - lastSampleMs) >= 1000;

        if (isEvent || isSample) {
            float primDist = radar.primaryIdx >= 0 ? radar.targets[radar.primaryIdx].distanceM : -1.0f;
            float primTtc = radar.primaryIdx >= 0 ? radar.targets[radar.primaryIdx].ttcS : INFINITY;
            // Raw per-target SNR byte, added 2026-09-21: analysing the first
            // real road test turned up an isolated CRITICAL at 38.7 km/h
            // claiming a target 2.3 m ahead, with "no target at all" in the
            // samples either side of it — physically implausible for a real
            // vehicle, but impossible to tell apart from a genuine
            // cut-in-at-close-range without knowing how strong the return
            // actually was. Logging it is what makes that distinction
            // possible next time.
            float primSnr = radar.primaryIdx >= 0 ? radar.targets[radar.primaryIdx].snr : -1.0f;
            const char *risk = radar.primaryIdx >= 0 ? riskLabelFor(primTtc, cfg.ttcWarnS, cfg.ttcCritS) : "SAFE";
            char ttcBuf[16];
            if (isinf(primTtc)) snprintf(ttcBuf, sizeof(ttcBuf), "-");
            else snprintf(ttcBuf, sizeof(ttcBuf), "%.2f", (double)primTtc);

            char line[128];
            snprintf(line, sizeof(line), "%lu,%.1f,%d,%.1f,%s,%.0f,%s,%d,%d,%s", (unsigned long)now,
                      (double)gnss.egoSpeedKmh, activeTargets, (double)primDist, ttcBuf, (double)primSnr, risk,
                      tooCloseNow ? 1 : 0, harshBrakeNow ? 1 : 0, isEvent ? "EVENT" : "");
            if (sdMgrAppendLine(logPath, line)) lastSampleMs = now;
        }

        wasCritical = criticalNow;
        wasTooClose = tooCloseNow;
        wasHarshBrake = harshBrakeNow;
    }
}

// 5120 — now an actually MEASURED size (2026-09-21), closing this comment's
// own previous "8192 is a generous first fix, not yet measured" note.
// History: the original guess of 3072 (copied from radar/LD2451.cpp's own
// measured figure without measuring THIS task) was a real stack-canary
// overflow panic on hardware 2026-09-16 ("Stack canary watchpoint triggered
// (tripLog)"), crash-looping the device every ~1-2s; 8192 stopped that but
// was deliberately over-generous. uxTaskGetStackHighWaterMark() on real
// hardware then read a rock-steady 4652 bytes FREE of those 8192 — i.e.
// this task's true peak is ~3540 bytes (which also explains the 3072 crash
// exactly: 3072 < 3540). 5120 keeps ~1580 bytes (~45%) margin over that
// measured peak — deliberately more headroom than the ~30% the other tasks
// here settled on, because this one's deepest path is ESP-IDF's VFS/FATFS
// *write* stack (sdMgrAppendLine()'s mkdir + open-append + println + close),
// whose worst case on an error/retry path isn't something a steady-state
// high-water reading can fully prove — and gives ~3KB of scarce internal
// RAM back versus 8192.
void tripLoggerStart() { xTaskCreatePinnedToCore(tripLoggerTaskFn, "tripLog", 5120, NULL, 1, NULL, 0); }
