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
#include <string.h> // memcpy (UBX MON-VER)
#include <stdlib.h> // atoi (GSV counts)

// Bench drive simulator state (see gnssSimStart in GNSS.h).
static volatile bool gSimActive = false;
static float gSimLat = 0, gSimLon = 0, gSimHeadingDeg = 0, gSimSpeedKmh = 0;
static uint32_t gSimEndMs = 0, gSimLastStepMs = 0, gSimFixSeq = 0;
// Waypoint mode (gnssSimRoute): follow a polyline instead of a straight line.
static const int kSimMaxWp = 48;
static float gSimWpLat[kSimMaxWp], gSimWpLon[kSimMaxWp];
static int gSimWpN = 0, gSimWpNext = 0;
void gnssSimRoute(const float *lat, const float *lon, int n, float speedKmh) {
    if (n < 2) return;
    if (n > kSimMaxWp) n = kSimMaxWp;
    for (int i = 0; i < n; i++) { gSimWpLat[i] = lat[i]; gSimWpLon[i] = lon[i]; }
    gSimWpN = n;
    gSimWpNext = 1;
    gSimLat = lat[0];
    gSimLon = lon[0];
    gSimSpeedKmh = speedKmh;
    float dy = (lat[1] - lat[0]) * 110540.0f, dx = (lon[1] - lon[0]) * 111320.0f * cosf(lat[0] * 0.0174533f);
    gSimHeadingDeg = fmodf(atan2f(dx, dy) * 57.29578f + 360.0f, 360.0f);
    gSimLastStepMs = millis();
    gSimEndMs = gSimLastStepMs + 3600000UL; // ends at the last waypoint
    gSimActive = true;
    Serial.printf("[gnss] route simulation: %d waypoints at %.0f km/h\n", n, (double)speedKmh);
}
void gnssSimStart(float lat, float lon, float headingDeg, float speedKmh, float seconds) {
    if (seconds <= 0) {
        gSimActive = false;
        Serial.println("[gnss] drive simulation stopped");
        return;
    }
    gSimWpN = 0;
    gSimLat = lat;
    gSimLon = lon;
    gSimHeadingDeg = headingDeg;
    gSimSpeedKmh = speedKmh;
    gSimLastStepMs = millis();
    gSimEndMs = gSimLastStepMs + (uint32_t)(seconds * 1000.0f);
    gSimActive = true;
    Serial.printf("[gnss] drive simulation: %.6f,%.6f hdg %.0f %.0f km/h for %.0f s\n", (double)lat, (double)lon,
                  (double)headingDeg, (double)speedKmh, (double)seconds);
}

// Confirmed on real hardware 2026-09-15 by sweeping candidate bauds and
// dumping raw bytes: this specific M10N breakout is NOT the common
// 9600-by-default kind — 9600 decoded as binary garbage (proved it wasn't
// a wiring fault), 38400 decoded clean, valid NMEA immediately (12 sats,
// real fix). Don't assume 9600 for a "default" M10N breakout again; verify
// per-board like this was.
// VietHUD 2.8's module runs at 115200 instead (pincfg.h GNSS_BAUD).
static const uint32_t kGnssBaud = GNSS_BAUD;

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

// ---------------------------------------------------------------------------
// u-blox UBX configuration (2026-09-26). Until now the module ran on its
// factory defaults (NMEA only, "portable" dynamics). At every boot we now:
//   * poll UBX-MON-VER (firmware + supported GNSS, logged for diagnosis);
//   * set the AUTOMOTIVE dynamic model (smoother, road-constrained fixes);
//   * enable AssistNow Autonomous (on-chip orbit prediction = A-GNSS without
//     internet; survives power-off only if the breakout has a backup supply);
//   * enable GPS + BeiDou (B1I) + Galileo (+ QZSS, SBAS) — see kSignals.
// Written to the RAM layer only: nothing persists in the module, so a power
// cycle always returns it to its known factory state.
// ---------------------------------------------------------------------------
struct UbxKv {
    uint32_t key;
    uint8_t size; // value bytes: 1, 2 or 4
    uint32_t val;
};

static void ubxSend(uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t len) {
    uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += hdr[i]; b += a; }
    for (uint16_t i = 0; i < len; i++) { a += pl[i]; b += a; }
    uint8_t ck[2] = {a, b};
    Serial2.write(hdr, 6);
    if (len) Serial2.write(pl, len);
    Serial2.write(ck, 2);
}

#if defined(VIETHUD_BOARD_ES3C28P)
// Sends "$<body>*CS\r\n" to the module (vendor NMEA-style commands).
static void gnssSendNmeaCmd(const char *body) {
    uint8_t cs = 0;
    for (const char *p = body; *p; p++) cs ^= (uint8_t)*p;
    Serial2.printf("$%s*%02X\r\n", body, cs);
}
#endif

static void ubxValset(const UbxKv *kv, int n) {
    static uint8_t pl[4 + 16 * 8];
    int len = 0;
    pl[len++] = 0x00; // version
    pl[len++] = 0x01; // layers: RAM only
    pl[len++] = 0x00;
    pl[len++] = 0x00;
    for (int i = 0; i < n && len + 8 <= (int)sizeof(pl); i++) {
        for (int k = 0; k < 4; k++) pl[len++] = (uint8_t)(kv[i].key >> (8 * k));
        for (int k = 0; k < kv[i].size; k++) pl[len++] = (uint8_t)(kv[i].val >> (8 * k));
    }
    ubxSend(0x06, 0x8A, pl, len); // UBX-CFG-VALSET
}

// UBX NAV-PVT -> NMEA bridge (2026-10-10, VietHUD 2.8). The 2.8's GPS is a
// "P18 Pro" drone module (reports M10 ROM SPG 5.10) that only outputs UBX
// NAV-PVT + NAV-SAT at ~10 Hz and cannot be configured at all — confirmed on
// the real unit: CFG-VALSET (RAM, and RAM+BBR+Flash), legacy CFG-MSG and
// CFG-RATE are ACKed but change nothing; CASIC $PCAS and MediaTek $PMTK get
// no reply. So the automotive dynamic model the 3.5" build sets on its M10
// can't be had here; instead this bridge does the equivalent work on the
// ESP32 side and feeds the result to the same TinyGPS instance as $GPRMC +
// $GPGGA (+ $xxGSV counts from NAV-SAT), leaving everything downstream
// untouched:
//   * all ~10 samples of each second are used, not just one — velocity is
//     averaged as a N/E vector (speed AND course, ~3x less noise), while the
//     output cadence stays the 1 Hz the speed filter / fix-timeout logic was
//     tuned on with the 3.5" NMEA module;
//   * fixes worse than kMaxHAccM horizontal accuracy are reported as no-fix,
//     so a wandering urban-canyon position can't map-match the wrong road;
//   * stationary hold: an averaged speed below the module's own speed
//     accuracy (and under kStaticHoldKmh) is reported as 0, which is what the
//     automotive model's static hold would do at a red light.
// Harmless on a module that does send NMEA (it never emits NAV-PVT unasked).
static const float kMaxHAccM = 50.0f;
static const float kStaticHoldKmh = 5.0f;

static void feedNmea(const char *body) {
    uint8_t cs = 0;
    for (const char *p = body; *p; p++) cs ^= (uint8_t)*p;
    char tail[6];
    snprintf(tail, sizeof(tail), "*%02X\r\n", cs);
    gps.encode('$');
    for (const char *p = body; *p; p++) gps.encode(*p);
    for (const char *p = tail; *p; p++) gps.encode(*p);
}

static void fmtCoord(char *out, size_t cap, int32_t e7, bool isLat) {
    char hemi = isLat ? (e7 < 0 ? 'S' : 'N') : (e7 < 0 ? 'W' : 'E');
    uint32_t a = (uint32_t)(e7 < 0 ? -(int64_t)e7 : e7);
    uint32_t deg = a / 10000000u;
    double minutes = (double)(a % 10000000u) * 60.0 / 1e7;
    snprintf(out, cap, isLat ? "%02lu%08.5f,%c" : "%03lu%08.5f,%c", (unsigned long)deg, minutes, hemi);
}

static inline int32_t ubxI32(const uint8_t *p, int o) {
    return (int32_t)((uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24));
}
static inline uint16_t ubxU16(const uint8_t *p, int o) { return (uint16_t)(p[o] | (p[o + 1] << 8)); }

// Latest per-second vertical speed from the bridge (+ = up), picked up by the
// GNSS task loop below into GnssSnapshot::vertSpeedMs. Same task, no locking.
static const float kMaxVertSAccMs = 1.0f; // speed accuracy above this = don't trust velD
static float sBridgeVertMs = 0;
static bool sBridgeVertValid = false;
static uint32_t sBridgeVertAtMs = 0;

static void bridgeNavPvt(const uint8_t *p) {
    // Per-second accumulators (good samples only).
    static double sVelN = 0, sVelE = 0, sVelD = 0, sSAcc = 0;
    static int sGood = 0;
    static uint8_t sLast[92];
    static bool sHaveLast = false;
    static uint32_t sWindowStartMs = 0;
    static bool sFirst = true;

    uint8_t fixType = p[20], flags = p[21];
    float hAccM = (uint32_t)ubxI32(p, 40) / 1000.0f;
    bool fixOk = (flags & 0x01) && (fixType == 2 || fixType == 3 || fixType == 4) && hAccM <= kMaxHAccM;
    if (fixOk) {
        sVelN += ubxI32(p, 48); // mm/s
        sVelE += ubxI32(p, 52);
        sVelD += ubxI32(p, 56); // + = down
        sSAcc += (uint32_t)ubxI32(p, 68);
        sGood++;
    }
    memcpy(sLast, p, sizeof(sLast));
    sHaveLast = true;

    // Emit about once a second on the local clock (iTOW sits at 0 until the
    // module has satellites, so it can't pace this).
    uint32_t now = millis();
    if (!sFirst && now - sWindowStartMs < 950) return;
    sFirst = false;
    sWindowStartMs = now;
    if (!sHaveLast) return;

    const uint8_t *q = sLast; // latest sample: position, time, sats, DOP
    uint16_t year = ubxU16(q, 4);
    uint8_t month = q[6], day = q[7], hh = q[8], mm = q[9], ss = q[10], valid = q[11], numSV = q[23];
    bool timeOk = (valid & 0x03) == 0x03; // validDate + validTime
    float lastHAccM = (uint32_t)ubxI32(q, 40) / 1000.0f;
    bool outFix = sGood > 0 && (q[21] & 0x01) && lastHAccM <= kMaxHAccM;

    double speedMs = 0, course = 0;
    if (sGood > 0) {
        double vn = sVelN / sGood / 1000.0, ve = sVelE / sGood / 1000.0;
        speedMs = sqrt(vn * vn + ve * ve);
        course = atan2(ve, vn) * 180.0 / M_PI;
        if (course < 0) course += 360.0;
        double sAccMs = sSAcc / sGood / 1000.0;
        if (speedMs * 3.6 < kStaticHoldKmh && speedMs < sAccMs) speedMs = 0; // static hold
        sBridgeVertMs = (float)(-sVelD / sGood / 1000.0);
        sBridgeVertValid = outFix && sAccMs <= kMaxVertSAccMs;
    } else {
        sBridgeVertValid = false;
    }
    sBridgeVertAtMs = now;
    sVelN = sVelE = sVelD = sSAcc = 0;
    sGood = 0;

    char tm[12] = "", dt[8] = "", lat[20] = ",", lon[20] = ",";
    if (timeOk) {
        snprintf(tm, sizeof(tm), "%02u%02u%02u.00", hh, mm, ss);
        snprintf(dt, sizeof(dt), "%02u%02u%02u", day, month, year % 100);
    }
    if (outFix) {
        fmtCoord(lat, sizeof(lat), ubxI32(q, 28), true);
        fmtCoord(lon, sizeof(lon), ubxI32(q, 24), false);
    }
    double dop = ubxU16(q, 76) / 100.0; // pDOP — closest thing NAV-PVT has to HDOP
    double alt = ubxI32(q, 36) / 1000.0; // hMSL

    char s[128];
    snprintf(s, sizeof(s), "GPRMC,%s,%c,%s,%s,%.2f,%.1f,%s,,,%c", tm, outFix ? 'A' : 'V', lat, lon,
             speedMs * 1.9438445, course, dt, outFix ? 'A' : 'N');
    feedNmea(s);
    if (outFix) {
        snprintf(s, sizeof(s), "GPGGA,%s,%s,%s,1,%02u,%.1f,%.1f,M,0.0,M,,", tm, lat, lon, numSV, dop, alt);
    } else {
        snprintf(s, sizeof(s), "GPGGA,%s,,,,,0,%02u,,,M,,M,,", tm, numSV);
    }
    feedNmea(s);
}

// Satellites actually used in the fix and their mean C/N0 (dB-Hz), from the
// latest NAV-SAT — logged only for now: a sudden drop is the most direct
// "driving under a viaduct" signal, but it should be tuned on real drive logs
// before it replaces the HDOP rule in SpeedLimitManager's skyBlocked.
static int sSatUsed = -1;
static float sSatCn0Mean = 0;

// NAV-SAT -> per-constellation "satellites in view" as minimal $xxGSV
// sentences (only field 3, the total, which is all the diagnostics read).
static void bridgeNavSat(const uint8_t *p, uint16_t n) {
    if (n < 8) return;
    uint8_t numSvs = p[5];
    int cnt[7] = {0}; // by gnssId: 0 GPS, 1 SBAS, 2 Galileo, 3 BeiDou, 5 QZSS, 6 GLONASS
    int used = 0, cn0Sum = 0;
    for (int i = 0; i < numSvs && 8 + 12 * i + 12 <= n; i++) {
        const uint8_t *sv = p + 8 + 12 * i;
        uint8_t g = sv[0];
        if (g < 7) cnt[g]++;
        if (sv[8] & 0x08) { // flags.svUsed
            used++;
            cn0Sum += sv[2];
        }
    }
    sSatUsed = used;
    sSatCn0Mean = used ? (float)cn0Sum / used : 0;
    static const struct { uint8_t id; const char *talker; } kMap[] = {
        {0, "GP"}, {6, "GL"}, {2, "GA"}, {3, "GB"}, {5, "GQ"}};
    for (const auto &m : kMap) {
        char s[32];
        snprintf(s, sizeof(s), "%sGSV,1,1,%02d", m.talker, cnt[m.id]);
        feedNmea(s);
    }
}

// Minimal UBX frame receiver interleaved with the NMEA stream (0xB5 never
// occurs in NMEA ASCII, so UBX bytes are cleanly separable from TinyGPS input).
static volatile int8_t sValsetAck = 0; // 0 pending, 1 ACK, -1 NAK
static bool ubxFeed(uint8_t c) {
    static uint8_t st = 0, cls, id, ckA, ckB;
    static uint16_t len, got;
    static uint8_t buf[8 + 12 * 48]; // NAV-SAT: 8-byte header + 12 per satellite
    switch (st) {
    case 0: if (c == 0xB5) { st = 1; return true; } return false;
    case 1: if (c == 0x62) { st = 2; ckA = ckB = 0; return true; } st = 0; return false;
    case 2: cls = c; ckA += c; ckB += ckA; st = 3; return true;
    case 3: id = c; ckA += c; ckB += ckA; st = 4; return true;
    case 4: len = c; ckA += c; ckB += ckA; st = 5; return true;
    case 5:
        len |= (uint16_t)c << 8; ckA += c; ckB += ckA; got = 0;
        st = len ? 6 : 7;
        return true;
    case 6:
        if (got < sizeof(buf)) buf[got] = c;
        got++; ckA += c; ckB += ckA;
        if (got >= len) st = 7;
        return true;
    case 7: st = (c == ckA) ? 8 : 0; return true;
    case 8: {
        st = 0;
        if (c != ckB) return true;
        uint16_t n = len < sizeof(buf) ? len : sizeof(buf);
        if (cls == 0x05 && n >= 2 && buf[0] == 0x06 && buf[1] == 0x8A) {
            sValsetAck = (id == 0x01) ? 1 : -1; // ACK-ACK / ACK-NAK for CFG-VALSET
        } else if (cls == 0x01 && id == 0x07 && n >= 92) { // NAV-PVT
            bridgeNavPvt(buf);
        } else if (cls == 0x01 && id == 0x35) { // NAV-SAT
            bridgeNavSat(buf, n);
        } else if (cls == 0x0A && id == 0x04 && n >= 40) { // MON-VER
            char sw[31], hw[11];
            memcpy(sw, buf, 30); sw[30] = 0;
            memcpy(hw, buf + 30, 10); hw[10] = 0;
            for (char *q = sw; *q; q++) if (*q < 0x20 || *q > 0x7E) *q = '?';
            for (char *q = hw; *q; q++) if (*q < 0x20 || *q > 0x7E) *q = '?';
            Serial.printf("[gnss] module: sw=\"%s\" hw=\"%s\"\n", sw, hw);
            for (uint16_t o = 40; o + 30 <= n; o += 30) {
                char ext[31];
                memcpy(ext, buf + o, 30); ext[30] = 0;
                Serial.printf("[gnss]   %s\n", ext);
            }
        }
        return true;
    }
    }
    st = 0;
    return false;
}

// Runs the configuration steps; called every loop tick until done.
static void ubxConfigStep(uint32_t sinceBootMs) {
    static int step = 0;
    static uint32_t sentAt = 0;
    static const UbxKv kBase[] = {
        {0x20110021, 1, 4}, // CFG-NAVSPG-DYNMODEL = 4 (automotive)
        {0x10230001, 1, 1}, // CFG-ANA-USE_ANA = 1 (AssistNow Autonomous)
    };
    // GPS + BeiDou B1I + Galileo (+ QZSS, SBAS). Verified on this module
    // (PROTVER 34.10): adding GLONASS is REJECTED — the M10's single RF path
    // can't receive GLONASS L1 (1602 MHz) together with BeiDou B1I (1561 MHz).
    // For Vietnam B1I is the better pick: it includes BeiDou's GEO/IGSO
    // satellites that sit permanently over Asia (GLONASS adds little here).
    static const UbxKv kSignals[] = {
        {0x1031001f, 1, 1}, {0x10310001, 1, 1}, // GPS L1C/A
        {0x10310022, 1, 1}, {0x1031000d, 1, 1}, // BeiDou B1I
        {0x10310021, 1, 1}, {0x10310007, 1, 1}, // Galileo E1
        {0x10310025, 1, 0},                     // GLONASS off (see above)
        {0x10310024, 1, 1}, {0x10310012, 1, 1}, // QZSS L1C/A
        {0x10310020, 1, 1}, {0x10310005, 1, 1}, // SBAS L1C/A
    };
    auto waitAck = [&](const char *what) -> int { // 1 ack, -1 nak/timeout, 0 still waiting
        if (sValsetAck == 0 && sinceBootMs - sentAt < 1500) return 0;
        int r = sValsetAck == 1 ? 1 : -1;
        Serial.printf("[gnss] config %s: %s\n", what, r == 1 ? "OK" : (sValsetAck == -1 ? "REJECTED" : "no answer"));
        return r;
    };
    switch (step) {
    case 0:
        if (sinceBootMs < 800) return;
#if defined(VIETHUD_BOARD_ES3C28P)
        // The P18 Pro's real config interface is a Quectel-style PQTM subset
        // (found 2026-10-10: answers $PQTMVERNO with "SUB,V01", firmware
        // "#VERSIONA,B01-V5.34.8"). Only message rates are writable there —
        // constellation/fix-rate/UART/nav-mode are rejected (ERROR,1) or
        // ignored. This build reads NAV-PVT (it carries hAcc/sAcc that the
        // bridge above gates on), so make sure native NMEA is off in case it
        // was left on: two position streams would be mixed into one TinyGPS.
        // RAM only (no $PQTMSAVEPAR), so the module stays stock for a drone.
        gnssSendNmeaCmd("PQTMCFGMSGRATE,W,RMC,0");
        gnssSendNmeaCmd("PQTMCFGMSGRATE,W,GGA,0");
#endif
        ubxSend(0x0A, 0x04, nullptr, 0); // poll MON-VER
        sentAt = sinceBootMs;
        step = 1;
        return;
    case 1:
        if (sinceBootMs - sentAt < 400) return;
        sValsetAck = 0; ubxValset(kBase, sizeof(kBase) / sizeof(kBase[0])); sentAt = sinceBootMs; step = 2;
        return;
    case 2:
        if (!waitAck("automotive + AssistNow Autonomous")) return;
        sValsetAck = 0; ubxValset(kSignals, sizeof(kSignals) / sizeof(kSignals[0])); sentAt = sinceBootMs; step = 3;
        return;
    case 3:
        if (!waitAck("GPS+BeiDou+Galileo+QZSS+SBAS")) return;
        step = 99;
        return;
    default:
        return;
    }
}

// Satellites IN VIEW per constellation (GSV field 3), for diagnostics — shows
// whether BeiDou/Galileo/GLONASS are actually being tracked.
static TinyGPSCustom gsvGps(gps, "GPGSV", 3), gsvGlo(gps, "GLGSV", 3), gsvGal(gps, "GAGSV", 3),
    gsvBds(gps, "GBGSV", 3), gsvQzs(gps, "GQGSV", 3);
static int gsvCount(TinyGPSCustom &c) { return c.isValid() ? atoi(c.value()) : 0; }

// File-scope (not function-local) so gnssMsSinceStationary() below can read
// it — see GNSS.h's comment on that function for what this tracks.
static uint32_t lastMovingMs = 0;

uint32_t gnssMsSinceStationary() { return millis() - lastMovingMs; }

static void gnssTaskFn(void *) {
    Serial2.begin(kGnssBaud, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);

    SpeedFilter speedFilter;
    const uint32_t taskStartMs = millis();
    esp_task_wdt_add(NULL);
    uint32_t lastFixMs = 0;
    uint32_t lastValidSentenceMs = 0;
    // Heading hold state (GNSS.h's kHeadingHoldMs comment) — lastKnownHeadingMs
    // == 0 means "nothing to hold yet", same sentinel style as lastFixMs.
    float lastKnownHeadingDeg = 0;
    uint32_t lastKnownHeadingMs = 0;
    uint32_t lastPassedChecksum = 0;
    uint32_t lastDebugMs = 0;

    // "No fix yet" and "not receiving anything" used to both show as FAULT
    // — misleading, since the first one is the normal state while cold-
    // starting/indoors and the second is an actual wiring/power/baud
    // problem. linkAlive tracks the second, separately from fix.
    const uint32_t kLinkTimeoutMs = 5000; // generous vs. NMEA's ~1s sentence burst cadence

    for (;;) {
        while (Serial2.available()) {
            uint8_t c = (uint8_t)Serial2.read();
            if (!ubxFeed(c)) gps.encode((char)c); // UBX replies are consumed here, NMEA goes to TinyGPS
        }
        ubxConfigStep(millis() - taskStartMs);

        uint32_t passedChecksum = gps.passedChecksum();
        if (passedChecksum != lastPassedChecksum) {
            lastPassedChecksum = passedChecksum;
            lastValidSentenceMs = millis();
        }
        bool linkAlive = lastValidSentenceMs != 0 && (millis() - lastValidSentenceMs) < kLinkTimeoutMs;

        bool freshFix = gps.location.isValid() && gps.location.isUpdated();
        static uint32_t sFixSeq = 0;
        if (freshFix) {
            lastFixMs = millis();
            sFixSeq++;
        }
        // GNSS considered "lost" if no fresh fix for this long — spec
        // section 23: never hold/assume a stale speed once lost, disable
        // the audio gate, show fault. Tunable from Settings > Sensors.
        uint32_t fixTimeoutMs = (uint32_t)(cfg.gnssFixTimeoutS * 1000.0f);
        bool haveRecentFix = gps.location.isValid() && (millis() - lastFixMs) < fixTimeoutMs;

        GnssSnapshot snap;
        snap.fix = haveRecentFix;
        snap.linkAlive = linkAlive;
        if (haveRecentFix && gps.speed.isValid()) {
            // Calibration applied here, before the filter, so both
            // rawSpeedKmh and the filtered egoSpeedKmh reflect the corrected
            // value (AppConfig.h's gnssSpeedCalibrationPct comment) — no
            // separate "true" vs "displayed" speed anywhere downstream.
            float measuredKmh = (float)gps.speed.kmph();
            snap.rawSpeedKmh = measuredKmh * (1.0f + cfg.gnssSpeedCalibrationPct / 100.0f);
            snap.egoSpeedKmh = speedFilter.push(snap.rawSpeedKmh);
        } else {
            speedFilter.reset();
            snap.rawSpeedKmh = 0;
            snap.egoSpeedKmh = 0; // consumers must gate on .fix, never trust this while !fix
        }
        snap.satCount = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
        snap.hdop = gps.hdop.isValid() ? (float)gps.hdop.hdop() : 0.0f;
        snap.fixSeq = sFixSeq;

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
        if (haveRecentFix && gps.altitude.isValid()) {
            snap.altitudeM = (float)gps.altitude.meters();
            snap.altitudeValid = true;
        }
        if (haveRecentFix && sBridgeVertValid && millis() - sBridgeVertAtMs < 2000) {
            snap.vertSpeedMs = sBridgeVertMs;
            snap.vertSpeedValid = true;
        }

        // Course over ground (spec architecture note: "M10N xác định vị trí,
        // tốc độ, hướng" — heading was the missing piece). A GPS module
        // derives course from the motion between fixes, not a compass, so
        // it's meaningless/noisy near-stationary — gated on a minimum speed,
        // same "don't trust a noisy low-signal reading" reasoning as the
        // speed filter's own median+EMA above. kGnssMotionThresholdKmh (see
        // GNSS.h) is this project's one shared constant for "real motion vs.
        // GPS noise floor" — also reused just below for the auto-dim gate.
        bool liveHeadingValid = haveRecentFix && gps.course.isValid() && snap.rawSpeedKmh > kGnssMotionThresholdKmh;
        if (liveHeadingValid) {
            snap.headingDeg = (float)gps.course.deg();
            snap.headingValid = true;
            snap.headingPredicted = false;
            lastKnownHeadingDeg = snap.headingDeg;
            lastKnownHeadingMs = millis();
        } else if (haveRecentFix && lastKnownHeadingMs != 0 &&
                   (millis() - lastKnownHeadingMs) < kHeadingHoldMs) {
            // Bridge a brief GPS-course gap (car slowed below
            // kGnssMotionThresholdKmh, or the module's own course briefly
            // glitched) by holding the last known heading — GNSS.h's
            // kHeadingHoldMs comment has the full "dự đoán hướng di chuyển
            // của xe" reasoning. Requires a still-current FIX, not just a
            // recent one: a real position jump (module re-acquiring after a
            // gap) must never carry a stale direction forward.
            snap.headingDeg = lastKnownHeadingDeg;
            snap.headingValid = true;
            snap.headingPredicted = true;
        } else {
            snap.headingValid = false;
            snap.headingPredicted = false;
        }
        // A real fix loss invalidates any held heading immediately — once
        // GPS comes back (e.g. after a tunnel), the car could easily be
        // pointed a different way than before the gap, so the next fix must
        // re-earn a live heading rather than resume holding the old one.
        if (!haveRecentFix) lastKnownHeadingMs = 0;

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

        if (gSimActive) {
            uint32_t nowS = millis();
            if ((int32_t)(nowS - gSimEndMs) >= 0) {
                gSimActive = false;
                Serial.println("[gnss] drive simulation finished");
            } else if (nowS - gSimLastStepMs >= 200) {
                float dt = (nowS - gSimLastStepMs) / 1000.0f;
                gSimLastStepMs = nowS;
                float dM = gSimSpeedKmh / 3.6f * dt;
                if (gSimWpN > 0) {
                    // consume dM along the polyline
                    while (dM > 0 && gSimWpNext < gSimWpN) {
                        float kx = 111320.0f * cosf(gSimLat * 0.0174533f);
                        float dy = (gSimWpLat[gSimWpNext] - gSimLat) * 110540.0f;
                        float dx = (gSimWpLon[gSimWpNext] - gSimLon) * kx;
                        float L = sqrtf(dx * dx + dy * dy);
                        if (L > 0.01f) gSimHeadingDeg = fmodf(atan2f(dx, dy) * 57.29578f + 360.0f, 360.0f);
                        if (L <= dM) {
                            gSimLat = gSimWpLat[gSimWpNext];
                            gSimLon = gSimWpLon[gSimWpNext];
                            gSimWpNext++;
                            dM -= L;
                        } else {
                            gSimLat += dy * (dM / L) / 110540.0f;
                            gSimLon += dx * (dM / L) / kx;
                            dM = 0;
                        }
                    }
                    if (gSimWpNext >= gSimWpN) {
                        gSimEndMs = nowS; // arrived: stop on the next pass
                        gSimWpN = 0;
                    }
                } else {
                    float hr = gSimHeadingDeg * 0.0174533f;
                    gSimLat += dM * cosf(hr) / 110540.0f;
                    gSimLon += dM * sinf(hr) / (111320.0f * cosf(gSimLat * 0.0174533f));
                }
                gSimFixSeq++;
            }
            if (gSimActive) {
                snap.fix = true;
                snap.linkAlive = true;
                snap.satCount = 14;
                snap.latDeg = gSimLat;
                snap.lonDeg = gSimLon;
                snap.egoSpeedKmh = snap.rawSpeedKmh = gSimSpeedKmh;
                snap.headingDeg = gSimHeadingDeg;
                snap.headingValid = true;
                snap.headingPredicted = false;
                snap.hdop = 0.9f;
                snap.altitudeValid = false;
                snap.fixSeq = 0x80000000u | gSimFixSeq;
            }
        }
        gnssPublish(snap);

        uint32_t now = millis();
        if (now - lastDebugMs > 3000) {
            lastDebugMs = now;
            Serial.printf("[gnss] fix=%d link=%d sats=%d speed=%.1fkm/h alt=%.1fm vs=%+.2fm/s%s hdop=%.1f "
                          "used=%d cn0=%.0f chars=%lu sentencesWithFix=%lu failedChecksum=%lu\n",
                          snap.fix, snap.linkAlive, snap.satCount, (double)snap.egoSpeedKmh,
                          (double)snap.altitudeM, (double)snap.vertSpeedMs, snap.vertSpeedValid ? "" : "(n/a)",
                          (double)snap.hdop, sSatUsed, (double)sSatCn0Mean,
                          gps.charsProcessed(), gps.sentencesWithFix(), gps.failedChecksum());
            static uint32_t sLastViewLogMs = 0;
            if (now - sLastViewLogMs > 10000) {
                sLastViewLogMs = now;
                Serial.printf("[gnss] in view: GPS=%d BeiDou=%d Galileo=%d GLONASS=%d QZSS=%d\n", gsvCount(gsvGps),
                              gsvCount(gsvBds), gsvCount(gsvGal), gsvCount(gsvGlo), gsvCount(gsvQzs));
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50)); // drain UART well inside NMEA's own ~1s sentence cadence
    }
}

// 3072, not the original 4096 — real-hardware measurement (2026-09-16 RAM
// audit): high-water mark never dropped below ~1944 bytes free of 4096, i.e. never used
// more than ~2152 bytes, leaving a comfortable ~920-byte (~30%) margin at
// 3072.
// 4096 since 2026-09-26: UBX configuration + MON-VER logging added to this task.
void gnssTaskStart() { xTaskCreatePinnedToCore(gnssTaskFn, "gnssTask", 4096, NULL, 2, NULL, 0); }
