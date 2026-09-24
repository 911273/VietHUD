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
    bool wasSpeeding = false, wasCameraAhead = false, wasSignVisible = false;
    uint32_t lastSampleMs = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!cfg.tripLoggingEnabled) continue;

        if (!headerWritten) {
            // Retried every tick until it succeeds (e.g. card not mounted
            // yet at boot) rather than giving up — cheap, and losing the
            // header line forever would make the CSV harder to parse later.
            if (sdMgrAppendLine(logPath,
                                 "tMs,egoKmh,latDeg,lonDeg,headingDeg,limitValid,limitKmh,speeding,cameraAheadM,signType,signDistM,event")) {
                headerWritten = true;
                // Why the PREVIOUS run ended, written into this run's file
                // — esp_reset_reason() is already read and printed at boot
                // by main_ui_demo.cpp, but Serial isn't attached in the
                // car; persisting it here is what makes a mid-drive reset
                // diagnosable after the fact.
                char reasonLine[64];
                snprintf(reasonLine, sizeof(reasonLine), "# boot, previous run ended: %s",
                          tripLogResetReasonStr());
                sdMgrAppendLine(logPath, reasonLine);
            } else {
                continue;
            }
        }

        GnssSnapshot gnss = gnssSnapshot();
        RoadInfoSnapshot road = roadInfoSnapshot();

        static const float kSpeedingMarginKmh = 5.0f; // same margin ui/Dashboard.cpp's own speeding check uses
        bool speedingNow = road.valid && gnss.fix && gnss.egoSpeedKmh > road.speedLimitKmh + kSpeedingMarginKmh;
        bool cameraAheadNow = road.cameraAheadValid;
        // Same sign-type numbering ui/Dashboard.cpp's currentSignType uses
        // (2=resident area, 3=no overtaking, 5=toll booth, 6=traffic light,
        // 0=none) — kept consistent so the two logs read the same way.
        int signType = 0;
        float signDistM = -1;
        if (road.residentAreaAheadValid) { signType = 2; signDistM = road.residentAreaAheadDistM; }
        else if (road.noOvertakingAheadValid) { signType = 3; signDistM = road.noOvertakingAheadDistM; }
        else if (road.tollBoothAheadValid) { signType = 5; signDistM = road.tollBoothAheadDistM; }
        else if (road.trafficLightAheadValid) { signType = 6; signDistM = road.trafficLightAheadDistM; }
        bool signVisibleNow = signType != 0;

        // Rising-edge only (not "while true") — one EVENT row marking the
        // moment a condition STARTS is what's worth finding again later; a
        // row every 200ms for the whole duration it stays true would just
        // bloat the file with the same fact repeated.
        bool isEvent = (speedingNow && !wasSpeeding) || (cameraAheadNow && !wasCameraAhead) ||
                       (signVisibleNow && !wasSignVisible);
        uint32_t now = millis();
        bool isSample = (now - lastSampleMs) >= 1000;

        if (isEvent || isSample) {
            char line[160];
            snprintf(line, sizeof(line), "%lu,%.1f,%.6f,%.6f,%.1f,%d,%.0f,%d,%.0f,%d,%.0f,%s", (unsigned long)now,
                      (double)gnss.egoSpeedKmh, (double)gnss.latDeg, (double)gnss.lonDeg,
                      gnss.headingValid ? (double)gnss.headingDeg : -1.0,
                      road.valid ? 1 : 0, road.valid ? (double)road.speedLimitKmh : -1.0,
                      speedingNow ? 1 : 0, cameraAheadNow ? (double)road.cameraAheadDistanceM : -1.0, signType,
                      (double)signDistM, isEvent ? "EVENT" : "");
            if (sdMgrAppendLine(logPath, line)) lastSampleMs = now;
        }

        wasSpeeding = speedingNow;
        wasCameraAhead = cameraAheadNow;
        wasSignVisible = signVisibleNow;
    }
}

// 5120 — measured stack size, unchanged from the radar-era task (this
// task's actual work per tick — a couple of snapshot reads and an
// snprintf/append — is lighter now that no per-target loop exists, so the
// old measured peak stays a safe, if slightly generous, upper bound).
void tripLoggerStart() { xTaskCreatePinnedToCore(tripLoggerTaskFn, "tripLog", 5120, NULL, 1, NULL, 0); }
