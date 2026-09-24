#include "DemoMode.h"
#include "core/SharedState.h"
#include "map/MapRenderer.h" // mapRendererComputeFromSegments()/mapRendererReset() — live background map, demo path
#include "map/SpeedMapFormat.h" // RoadSegment, DIR_BIDIRECTIONAL, SPEED_SOURCE_OSM_MAXSPEED
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

// Which alert a scene drives. Mirrors ui/Dashboard.cpp's own AlertKind
// priority order, but kept as its own enum rather than sharing that one:
// Dashboard's is a private UI detail, and this module talks to the UI only
// through RoadInfoSnapshot's public fields — exactly like the real
// map/SpeedLimitManager.cpp does.
enum DemoAlert {
    D_NONE = 0,
    D_AHEAD_LIMIT,
    D_CAMERA,
    D_RESIDENT,
    D_NO_OVERTAKE,
    D_TRAFFIC_LIGHT,
    D_TOLL,
    D_DANGER,
};

// One step of the script. Distances interpolate linearly from distFrom to
// distTo across the scene's duration, so a countdown visibly ticks down
// through all three proximity tiers (FAR -> NEAR -> CLOSE) the way it would
// on a real approach — that escalation is the main thing there was no way to
// look at before.
struct DemoScene {
    const char *name;
    uint32_t durMs;
    float speedKmh;   // < 0 = report "no GNSS fix"
    float limitKmh;   // < 0 = no speed limit matched
    uint8_t alert;    // DemoAlert
    float distFrom, distTo;
    float alertValue; // ahead-limit / camera speed in km/h, < 0 = unknown
    bool isStart;     // resident-area / no-overtaking start vs. end variant
    const char *roadName; // Realistic road name
};

// Ordered to walk the UI from its emptiest state to its loudest, then through
// each sign type. Durations are long enough to actually read the screen and
// hear the voice line finish, short enough that a full lap is about a minute.
static const DemoScene kScenes[] = {
    {"Chua co GPS",        4000,  -1,  -1, D_NONE,          0,   0,  -1, true,  ""},
    {"Dung yen, GH 60",    4000,   0,  60, D_NONE,          0,   0,  -1, true,  "D. Dai Co Viet"},
    {"Chay 62, GH 60",     4000,  62,  60, D_NONE,          0,   0,  -1, true,  "D. Dai Co Viet"},
    {"Qua toc do 78/60",   6000,  78,  60, D_NONE,          0,   0,  -1, true,  "Ham Kim Lien"},
    {"GH truoc 40, 100m",  9000,  70,  60, D_AHEAD_LIMIT, 100,   0,  40, true,  "D. Xa Dan"},
    {"Camera 100->20m",   14000,  64,  60, D_CAMERA,      100,  20,  60, true,  "D. Nguyen Trai"},
    {"Camera khong GH",    7000,  55,  -1, D_CAMERA,      100,  20,  -1, true,  "D. Nguyen Trai"},
    {"Vao khu dan cu",    10000,  58,  60, D_RESIDENT,    350,  20,  -1, true,  "D. Nguyen Trai"},
    {"Het khu dan cu",     7000,  52,  50, D_RESIDENT,    300,  40,  -1, false, "QL 1A - Giai Phong"},
    {"Cam vuot",          10000,  61,  60, D_NO_OVERTAKE, 350,  20,  -1, true,  "QL 1A - Giai Phong"},
    {"Het cam vuot",       6000,  63,  60, D_NO_OVERTAKE, 250,  40,  -1, false, "QL 1A - Giai Phong"},
    {"Den tin hieu",      10000,  45,  50, D_TRAFFIC_LIGHT, 350, 15, -1, true,  "Dai lo Thang Long"},
    {"Tram thu phi",       8000,  70,  80, D_TOLL,        350,  40,  -1, true,  "Cao toc Phap Van"},
    {"Doan nguy hiem",     9000,  62,  60, D_DANGER,      350,  30,  -1, true,  "Deo Hai Van"},
};
static const int kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

// ---------------------------------------------------------------------
// Live background map demo data (user-requested 2026-09-22, "tuong thich
// hoan toan voi che do chay thu nghiem DemoMode hien co ... khong can ra
// duong"). A small hand-built road network anchored at this scene table's
// own base coordinate (21.0278N/105.8342E, "Hanoi" per applyScene()'s own
// comment) — one long "highway" through the middle (id 4242, matching
// applyScene()'s own hardcoded r.roadId so the map's "currently on this
// road" cyan highlight — map/MapRenderer.cpp's kMapLineCurrentRoad — lights
// up during the demo exactly the way it would on a real match), a
// "main road" crossing it, and a small "alley" loop off to one side, so all
// three road classes and the highlight all have something to render.
//
// NOT geographically load-bearing: the fake ego position below drifts well
// away from this tiny ~300-600m network within a lap or two (see
// updateDemoPosition()'s own comment on why that's an accepted, deliberate
// simplification) — the point of this data is exercising the real
// projection/rotation/class-coloring code paths, not simulating an
// accurate route.
//
// Coordinates as plain degrees here for readability; converted to the
// database's E7 (degrees x 1e7) fixed-point at static-init time.
static float e7(float deg) { return deg * 1e7f; } // helper only for THIS table's own readability, not a project-wide convention
#define DEMO_LAT 21.0278f
#define DEMO_LON 105.8342f

// Complete urban grid network centered around Hanoi (21.0278N, 105.8342E)
// Contains Highways (Class 1), Main Avenues (Class 2), and Connecting Streets (Class 3)
// Connected in a closed loop circuit so the car drives continuously along real roads forever!
#define D_LAT0 21.0240f
#define D_LAT1 21.0278f
#define D_LAT2 21.0316f
#define D_LON0 105.8290f
#define D_LON1 105.8342f
#define D_LON2 105.8394f

static const RoadSegment kDemoNetwork[] = {
    // --- Outer Loop Circuit ---
    // Leg 0: South Highway (Eastbound, Class 1, 80 km/h) -> ID 4242 & 4243
    {4242, (int32_t)e7(D_LAT0), (int32_t)e7(D_LON0), (int32_t)e7(D_LAT0), (int32_t)e7(D_LON1), 90, 1, DIR_BIDIRECTIONAL, 80, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4243, (int32_t)e7(D_LAT0), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT0), (int32_t)e7(D_LON2), 90, 1, DIR_BIDIRECTIONAL, 80, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // Leg 1: East Avenue (Northbound, Class 2, 60 km/h) -> ID 4300 & 4301
    {4300, (int32_t)e7(D_LAT0), (int32_t)e7(D_LON2), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON2), 0, 2, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4301, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON2), (int32_t)e7(D_LAT2), (int32_t)e7(D_LON2), 0, 2, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // Leg 2: North Boulevard (Westbound, Class 1, 70 km/h) -> ID 4250 & 4251
    {4250, (int32_t)e7(D_LAT2), (int32_t)e7(D_LON2), (int32_t)e7(D_LAT2), (int32_t)e7(D_LON1), 270, 1, DIR_BIDIRECTIONAL, 70, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4251, (int32_t)e7(D_LAT2), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT2), (int32_t)e7(D_LON0), 270, 1, DIR_BIDIRECTIONAL, 70, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // Leg 3: West Ring Road (Southbound, Class 2, 60 km/h) -> ID 4310 & 4311
    {4310, (int32_t)e7(D_LAT2), (int32_t)e7(D_LON0), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON0), 180, 2, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4311, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON0), (int32_t)e7(D_LAT0), (int32_t)e7(D_LON0), 180, 2, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // --- Cross City Streets ---
    // Central East-West Street (Class 2, 50 km/h) -> ID 4320 & 4321
    {4320, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON0), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1), 90, 2, DIR_BIDIRECTIONAL, 50, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4321, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON2), 90, 2, DIR_BIDIRECTIONAL, 50, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // Central North-South Street (Class 2, 50 km/h) -> ID 4330 & 4331
    {4330, (int32_t)e7(D_LAT0), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1), 0, 2, DIR_BIDIRECTIONAL, 50, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4331, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT2), (int32_t)e7(D_LON1), 0, 2, DIR_BIDIRECTIONAL, 50, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // Secondary connector streets (Class 3, 40 km/h)
    {4401, (int32_t)e7(D_LAT0 + 0.0019f), (int32_t)e7(D_LON0), (int32_t)e7(D_LAT0 + 0.0019f), (int32_t)e7(D_LON1), 90, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4402, (int32_t)e7(D_LAT0 + 0.0019f), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT0 + 0.0019f), (int32_t)e7(D_LON2), 90, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4403, (int32_t)e7(D_LAT1 + 0.0019f), (int32_t)e7(D_LON0), (int32_t)e7(D_LAT1 + 0.0019f), (int32_t)e7(D_LON1), 90, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4404, (int32_t)e7(D_LAT1 + 0.0019f), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT1 + 0.0019f), (int32_t)e7(D_LON2), 90, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // North-South dividers
    {4405, (int32_t)e7(D_LAT0), (int32_t)e7(D_LON0 + 0.0026f), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON0 + 0.0026f), 0, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4406, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON0 + 0.0026f), (int32_t)e7(D_LAT2), (int32_t)e7(D_LON0 + 0.0026f), 0, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4407, (int32_t)e7(D_LAT0), (int32_t)e7(D_LON1 + 0.0026f), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1 + 0.0026f), 0, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4408, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1 + 0.0026f), (int32_t)e7(D_LAT2), (int32_t)e7(D_LON1 + 0.0026f), 0, 3, DIR_BIDIRECTIONAL, 40, SPEED_SOURCE_OSM_MAXSPEED, 0},

    // Express Diagonal interchange ramps
    {4450, (int32_t)e7(D_LAT1 - 0.0010f), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1 + 0.0015f), 55, 1, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4451, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1 + 0.0015f), (int32_t)e7(D_LAT1 + 0.0010f), (int32_t)e7(D_LON1), 305, 1, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4452, (int32_t)e7(D_LAT1 + 0.0010f), (int32_t)e7(D_LON1), (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1 - 0.0015f), 235, 1, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},
    {4453, (int32_t)e7(D_LAT1), (int32_t)e7(D_LON1 - 0.0015f), (int32_t)e7(D_LAT1 - 0.0010f), (int32_t)e7(D_LON1), 125, 1, DIR_BIDIRECTIONAL, 60, SPEED_SOURCE_OSM_MAXSPEED, 0},
};
static const int kDemoNetworkCount = sizeof(kDemoNetwork) / sizeof(kDemoNetwork[0]);

// Realistic continuous closed-loop path integration:
// Dimensions: Width = (D_LON2 - D_LON0) * 111320 * cos(21) ~ 1080m
//             Height = (D_LAT2 - D_LAT0) * 110540 ~ 840m
// Total perimeter = 2 * (1080 + 840) = 3840 meters!
static const float kLeg0LenM = 1080.0f; // Eastbound
static const float kLeg1LenM = 840.0f;  // Northbound
static const float kLeg2LenM = 1080.0f; // Westbound
static const float kLeg3LenM = 840.0f;  // Southbound
static const float kCircuitPerimeterM = kLeg0LenM + kLeg1LenM + kLeg2LenM + kLeg3LenM;

static float gDemoTrackDist = 0.0f;
static uint32_t gDemoLastMoveMs = 0;
static uint32_t gDemoActiveRoadId = 4242;

static void updateDemoPosition(float speedKmh, GnssSnapshot &g) {
    uint32_t now = millis();
    float dtSec = gDemoLastMoveMs ? (now - gDemoLastMoveMs) / 1000.0f : 0.0f;
    gDemoLastMoveMs = now;
    if (dtSec > 0.5f) dtSec = 0.1f;

    float effectiveSpeed = speedKmh > 0 ? speedKmh : 0;
    gDemoTrackDist += (effectiveSpeed / 3.6f) * dtSec;
    if (gDemoTrackDist >= kCircuitPerimeterM) {
        gDemoTrackDist = fmodf(gDemoTrackDist, kCircuitPerimeterM);
    }

    float d = gDemoTrackDist;
    float lat = D_LAT0, lon = D_LON0, heading = 90.0f;

    if (d < kLeg0LenM) {
        // Leg 0: Eastbound on South Highway
        float frac = d / kLeg0LenM;
        lat = D_LAT0;
        lon = D_LON0 + frac * (D_LON2 - D_LON0);
        heading = 90.0f;
        gDemoActiveRoadId = (frac < 0.5f) ? 4242 : 4243;
    } else if (d < kLeg0LenM + kLeg1LenM) {
        // Leg 1: Northbound on East Avenue
        float frac = (d - kLeg0LenM) / kLeg1LenM;
        lat = D_LAT0 + frac * (D_LAT2 - D_LAT0);
        lon = D_LON2;
        heading = 0.0f;
        gDemoActiveRoadId = (frac < 0.5f) ? 4300 : 4301;
    } else if (d < kLeg0LenM + kLeg1LenM + kLeg2LenM) {
        // Leg 2: Westbound on North Boulevard
        float frac = (d - (kLeg0LenM + kLeg1LenM)) / kLeg2LenM;
        lat = D_LAT2;
        lon = D_LON2 - frac * (D_LON2 - D_LON0);
        heading = 270.0f;
        gDemoActiveRoadId = (frac < 0.5f) ? 4250 : 4251;
    } else {
        // Leg 3: Southbound on West Ring Road
        float frac = (d - (kLeg0LenM + kLeg1LenM + kLeg2LenM)) / kLeg3LenM;
        lat = D_LAT2 - frac * (D_LAT2 - D_LAT0);
        lon = D_LON0;
        heading = 180.0f;
        gDemoActiveRoadId = (frac < 0.5f) ? 4310 : 4311;
    }

    g.latDeg = lat;
    g.lonDeg = lon;
    g.headingDeg = heading;
}

static volatile bool gEnabled = false;
static volatile int gSceneIdx = 0;

bool demoModeIsEnabled() { return gEnabled; }

const char *demoModeSceneName() {
    if (!gEnabled) return "OFF";
    int i = gSceneIdx;
    if (i < 0 || i >= kSceneCount) return "--";
    return kScenes[i].name;
}

void demoModeSetEnabled(bool on) {
    if (on == gEnabled) return;
    gEnabled = on;
    gSceneIdx = 0;
    // Reset the fake ego position/timing too — otherwise re-enabling the
    // demo later would resume from wherever the synthetic path had drifted
    // to last time instead of a clean start at the network's own anchor.
    gDemoTrackDist = 0.0f;
    gDemoLastMoveMs = 0;
    gDemoActiveRoadId = 4242;
    Serial.printf("[demo] %s\n", on ? "ON — publishing synthetic GNSS/road data" : "OFF — real GNSS/map resumed");
    if (!on) {
        // Hand back a clean slate rather than leaving the last fake scene
        // frozen on screen: the real GNSS task republishes within ~50ms, and
        // the map task republishes on its own ~500ms tick (or never, if no
        // database is loaded — hence clearing road info explicitly here).
        RoadInfoSnapshot blank;
        roadInfoPublish(blank);
        mapRendererReset(); // clears the real map's own breadcrumb history too — see its own header comment
    }
}

static void applyScene(const DemoScene &sc, float t) {
    float dist = sc.distFrom + (sc.distTo - sc.distFrom) * t;

    GnssSnapshot g;
    g.fix = sc.speedKmh >= 0;
    g.linkAlive = true;
    g.satCount = g.fix ? 11 : 0;
    g.egoSpeedKmh = g.fix ? sc.speedKmh : 0;
    g.rawSpeedKmh = g.egoSpeedKmh;
    g.headingValid = g.fix && sc.speedKmh > 3.0f;
    // Position/heading now actually move (see updateDemoPosition()'s own
    // comment) — was a hardcoded static point, which gave the live
    // background map's heading-up rotation and breadcrumb trail nothing to
    // show. Still anchored near Hanoi (21.0278N/105.8342E) so the
    // longitude-derived local clock reads sensibly (UTC+7) and stays close
    // to kDemoNetwork's own footprint.
    if (g.fix) {
        updateDemoPosition(sc.speedKmh, g);
    } else {
        g.latDeg = D_LAT0;
        g.lonDeg = D_LON0;
        g.headingDeg = 90.0f;
        gDemoLastMoveMs = 0;
    }
    g.timeValid = g.fix;
    g.utcHour = 7;
    g.utcMinute = 32;
    g.daytime = true;
    gnssPublish(g);

    RoadInfoSnapshot r;
    r.mapLoaded = true; // the whole point: pretend a database IS loaded
    if (sc.roadName && sc.roadName[0]) {
        strncpy(r.roadName, sc.roadName, sizeof(r.roadName) - 1);
        r.roadName[sizeof(r.roadName) - 1] = '\0';
    } else {
        r.roadName[0] = '\0';
    }
    if (sc.limitKmh >= 0) {
        r.valid = true;
        r.speedLimitKmh = sc.limitKmh;
        r.confidence = 0.92f;
        r.roadId = gDemoActiveRoadId;
        r.matchDistanceM = 3.1f;
        r.source = 1; // SPEED_SOURCE_OSM_MAXSPEED
    }
    switch (sc.alert) {
        case D_AHEAD_LIMIT:
            r.aheadLimitValid = true;
            r.aheadSpeedLimitKmh = sc.alertValue;
            r.aheadDistanceM = dist;
            break;
        case D_CAMERA:
            r.cameraAheadValid = true;
            r.cameraAheadDistanceM = dist;
            r.cameraSpeedLimitKmh = sc.alertValue;
            break;
        case D_RESIDENT:
            r.residentAreaAheadValid = true;
            r.residentAreaAheadDistM = dist;
            r.residentAreaIsStart = sc.isStart;
            break;
        case D_NO_OVERTAKE:
            r.noOvertakingAheadValid = true;
            r.noOvertakingAheadDistM = dist;
            r.noOvertakingIsStart = sc.isStart;
            break;
        case D_TRAFFIC_LIGHT:
            r.trafficLightAheadValid = true;
            r.trafficLightAheadDistM = dist;
            break;
        case D_TOLL:
            r.tollBoothAheadValid = true;
            r.tollBoothAheadDistM = dist;
            break;
        case D_DANGER:
            r.dangerAheadValid = true;
            r.dangerAheadDistM = dist;
            break;
        default:
            break;
    }
    roadInfoPublish(r);

    // Live background map — the demo's own path into
    // map/MapRenderer.cpp's real projection/rotation code, bypassing its
    // usual SD-backed fetch (mapRendererUpdate()) in favor of the small
    // synthetic kDemoNetwork above (see MapRenderer.h's own comment on why
    // this pure entry point exists). Reads roadInfoSnapshot() internally to
    // decide the current-road highlight, so this must run AFTER
    // roadInfoPublish(r) just above, not before.
    mapRendererComputeFromSegments(g, kDemoNetwork, kDemoNetworkCount);
}

static void demoTaskFn(void *) {
    uint32_t sceneStartMs = millis();
    for (;;) {
        if (!gEnabled) {
            sceneStartMs = millis();
            gSceneIdx = 0;
            vTaskDelay(pdMS_TO_TICKS(250)); // idle cheaply; nothing to do until switched on
            continue;
        }

        const DemoScene &sc = kScenes[gSceneIdx];
        uint32_t elapsed = millis() - sceneStartMs;
        if (elapsed >= sc.durMs) {
            gSceneIdx = (gSceneIdx + 1) % kSceneCount;
            sceneStartMs = millis();
            Serial.printf("[demo] scene: %s\n", kScenes[gSceneIdx].name);
            elapsed = 0;
        }
        applyScene(sc, sc.durMs ? (float)elapsed / (float)sc.durMs : 0.0f);

        // 100ms: fast enough that an interpolated countdown moves smoothly to
        // the eye, slow enough to stay well under the Dashboard's own 150ms
        // render tick (publishing faster would just be discarded unseen).
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Priority 1 and 2560 bytes of stack: this only ever formats a few floats
// into two small structs and publishes them, with no file I/O or parsing —
// far less demanding than gnss/GNSS.cpp's own 3072-byte task, which does full
// NMEA decoding. Pinned to core 0 alongside the other producer tasks, leaving
// core 1's loop() to the LVGL render it feeds.
void demoModeStart() { xTaskCreatePinnedToCore(demoTaskFn, "demoTask", 2560, NULL, 1, NULL, 0); }
