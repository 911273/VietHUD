#include "SpeedLimitMap.h"
#include "core/SharedState.h"
#include "pincfg.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h> // sscanf() — used directly in loadSegments()

struct SpeedLimitSegment {
    float lat, lon;
    float radiusM;
    float speedLimitKmh;
};

// Fixed-size, not a dynamic container: same reasoning MAX_TARGETS uses in
// SharedState.h — a small bench/demo dataset, not a real regional map. 200
// segments is plenty for testing a single test route; revisit (SPIFFS-style
// streaming lookup instead of loading everything into RAM) before this is
// ever pointed at a real city-scale map file.
static const int kMaxSegments = 200;
static SpeedLimitSegment segments[kMaxSegments];
static int segmentCount = 0;

static float haversineM(float lat1, float lon1, float lat2, float lon2) {
    const float kEarthR = 6371000.0f;
    float dLat = (lat2 - lat1) * (float)M_PI / 180.0f;
    float dLon = (lon2 - lon1) * (float)M_PI / 180.0f;
    float a = sinf(dLat / 2) * sinf(dLat / 2) +
              cosf(lat1 * (float)M_PI / 180.0f) * cosf(lat2 * (float)M_PI / 180.0f) * sinf(dLon / 2) * sinf(dLon / 2);
    float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    return kEarthR * c;
}

// Parses "/speedlimits.csv" off the already-mounted SD card — see
// SpeedLimitMap.h for the file format. Malformed/header/comment lines are
// silently skipped rather than aborting the whole load: one bad row
// shouldn't cost every segment after it, same "a transient bad read doesn't
// invalidate the whole stream" reasoning radar/LD2451.cpp's frame parser
// already applies to individual UART frames.
static void loadSegments() {
    File f = SD.open("/speedlimits.csv");
    if (!f) {
        Serial.println("[map] /speedlimits.csv not found on SD card root");
        return;
    }
    int skipped = 0;
    while (f.available() && segmentCount < kMaxSegments) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line[0] == '#') continue;
        float lat, lon, radius, limit;
        if (sscanf(line.c_str(), "%f,%f,%f,%f", &lat, &lon, &radius, &limit) != 4) {
            skipped++;
            continue;
        }
        segments[segmentCount++] = {lat, lon, radius, limit};
    }
    f.close();
    Serial.printf("[map] loaded %d speed-limit segments (%d lines skipped) from /speedlimits.csv\n", segmentCount,
                  skipped);
}

static void mapTaskFn(void *) {
    // A dedicated SPI bus instance on the SD pins, not the default SPI
    // object — this board's LCD is on a separate QSPI bus (see
    // display/DisplayDriver.cpp's Arduino_ESP32QSPI), so there's no
    // contention, but using the default global `SPI` here would silently
    // assume pins that were never confirmed for this purpose either.
    static SPIClass sdSpi(HSPI);
    sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    RoadInfoSnapshot ri; // defaults: mapLoaded=false, valid=false — see SpeedLimitMap.h's HONESTY NOTE
    if (!SD.begin(SD_CS_PIN, sdSpi)) {
        Serial.println("[map] SD card not detected (SD.begin() failed) — speed-limit lookup stays unavailable");
    } else {
        loadSegments();
        ri.mapLoaded = segmentCount > 0;
    }
    roadInfoPublish(ri);

    esp_task_wdt_add(NULL);
    for (;;) {
        if (ri.mapLoaded) {
            GnssSnapshot gnss = gnssSnapshot();
            RoadInfoSnapshot out;
            out.mapLoaded = true;
            if (gnss.fix) {
                float bestDist = 1e9f;
                int bestIdx = -1;
                for (int i = 0; i < segmentCount; i++) {
                    float d = haversineM(gnss.latDeg, gnss.lonDeg, segments[i].lat, segments[i].lon);
                    if (d <= segments[i].radiusM && d < bestDist) {
                        bestDist = d;
                        bestIdx = i;
                    }
                }
                if (bestIdx >= 0) {
                    out.valid = true;
                    out.speedLimitKmh = segments[bestIdx].speedLimitKmh;
                    out.matchDistanceM = bestDist;
                }
            }
            roadInfoPublish(out);
        }

        esp_task_wdt_reset();
        // 500ms while there's real work to do — matches Settings.cpp's own
        // "live values only need to be as fresh as a human reads them"
        // reasoning; a road segment doesn't change fast enough to need
        // anything tighter. 2s instead once mapLoaded is permanently false
        // (it can't become true later without a reboot — see
        // SpeedLimitMap.h's HONESTY NOTE), since there's nothing to
        // recompute at all, same idle-slower reasoning
        // net/WebPortal.cpp's webTaskFn uses for its own OFF-state delay.
        vTaskDelay(pdMS_TO_TICKS(ri.mapLoaded ? 500 : 2000));
    }
}

void mapTaskStart() { xTaskCreatePinnedToCore(mapTaskFn, "mapTask", 4096, NULL, 1, NULL, 0); }
