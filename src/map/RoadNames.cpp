#include "RoadNames.h"
#include "SdCardManager.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// File layout: see tools/map_builder/build_road_names.py.
namespace {
struct Header {
    char magic[4];
    uint16_t version, cellE5;
    int32_t minLatE5, minLonE5;
    uint16_t rows, cols;
    uint32_t nameCount, nameIndexOff, nameBlobOff, cellTableOff, dataOff, reserved;
} __attribute__((packed));
static_assert(sizeof(Header) == 44, "VHRN header");

const uint32_t kQueryIntervalMs = 700;
const uint32_t kLostTimeoutMs = 8000; // keep the last name this long off-road (tunnels, GPS drift)
const float kMaxSnapM = 30.0f;        // farther than this from any road = not on a known road
const float kClassPenaltyM = 1.2f;    // per class step: motorway 0 ... residential 6, links 7, service 8
const float kSwitchMarginM = 6.0f;    // a challenger must beat the current road by this much ...
const uint8_t kSwitchHits = 3;        // ... this many queries in a row (~2 s)
const float kMPerE5 = 1.11195f;       // metres per 1e-5 deg of latitude
const size_t kBlockMax = 96 * 1024;   // cap on the cached 3x3 block (dense city centre ~20-40 KB)

char gPath[80] = "";
bool gTried = false, gOk = false;
Header gH;

// Cached 3x3 block of cells around the car: raw run data per cell.
int32_t gBlockR = -1, gBlockC = -1;
uint8_t *gBlock = nullptr;
size_t gBlockLen = 0;
struct CellSpan { uint32_t off, len; int32_t r, c; };
CellSpan gSpans[9];
int gSpanCount = 0;

int64_t gCurrent = -1, gCandidate = -1;
uint8_t gCandidateHits = 0;
uint32_t gLastQueryMs = 0, gLastOnRoadMs = 0;
char gName[96] = "";

bool openFile() {
    if (gTried) return gOk;
    gTried = true;
    snprintf(gPath, sizeof(gPath), "%s/roadnames.bin", sdMgrGetBaseDir());
    if (!sdMgrReadBytes(gPath, 0, (uint8_t *)&gH, sizeof(gH)) || memcmp(gH.magic, "VHRN", 4) != 0 || gH.version != 1 ||
        gH.cellE5 == 0 || gH.rows == 0 || gH.cols == 0) {
        Serial.printf("[roadnames] %s not present — street names from the speed map only\n", gPath);
        return gOk = false;
    }
    if (!gBlock) gBlock = (uint8_t *)heap_caps_malloc(kBlockMax, MALLOC_CAP_SPIRAM);
    if (!gBlock) return gOk = false;
    Serial.printf("[roadnames] %s: %lu names, %ux%u cells of %.2f deg\n", gPath, (unsigned long)gH.nameCount, gH.rows,
                  gH.cols, gH.cellE5 / 1e5);
    return gOk = true;
}

// Loads the 3x3 cells around (r, c): one table read + one data read per row.
void loadBlock(int32_t r, int32_t c) {
    gBlockR = r;
    gBlockC = c;
    gBlockLen = 0;
    gSpanCount = 0;
    for (int32_t rr = r - 1; rr <= r + 1; rr++) {
        if (rr < 0 || rr >= gH.rows) continue;
        int32_t c0 = c - 1 < 0 ? 0 : c - 1, c1 = c + 1 >= gH.cols ? gH.cols - 1 : c + 1;
        uint32_t t[4] = {0};
        int n = c1 - c0 + 1;
        uint32_t idx = (uint32_t)rr * gH.cols + c0;
        if (!sdMgrReadBytes(gPath, gH.cellTableOff + idx * 4, (uint8_t *)t, (n + 1) * 4)) continue;
        uint32_t len = t[n] - t[0];
        if (len == 0) continue;
        if (gBlockLen + len > kBlockMax) {
            Serial.printf("[roadnames] block over %u KB, row skipped\n", (unsigned)(kBlockMax / 1024));
            continue;
        }
        if (!sdMgrReadBytes(gPath, t[0], gBlock + gBlockLen, len)) continue;
        for (int k = 0; k < n; k++) {
            if (t[k + 1] == t[k]) continue;
            gSpans[gSpanCount++] = {(uint32_t)(gBlockLen + (t[k] - t[0])), t[k + 1] - t[k], rr, c0 + k};
        }
        gBlockLen += len;
    }
}

float angleDiff180(float a, float b) {
    float d = fabsf(fmodf(a - b, 180.0f));
    return d > 90.0f ? 180.0f - d : d;
}

// Best road near the car: name index and score; also the current road's score.
void search(int32_t latE5, int32_t lonE5, float headingDeg, bool useHeading, int64_t prefer, int64_t &bestName,
            float &bestScore, float &curScore) {
    bestName = -1;
    bestScore = 1e9f;
    curScore = 1e9f;
    const float ky = kMPerE5, kx = kMPerE5 * cosf(latE5 * 1e-5f * (float)DEG_TO_RAD);
    for (int s = 0; s < gSpanCount; s++) {
        const CellSpan &sp = gSpans[s];
        const float carY = (float)(latE5 - (gH.minLatE5 + sp.r * (int32_t)gH.cellE5));
        const float carX = (float)(lonE5 - (gH.minLonE5 + sp.c * (int32_t)gH.cellE5));
        const uint8_t *p = gBlock + sp.off, *end = p + sp.len;
        while (p + 6 <= end) {
            uint32_t nameIdx;
            memcpy(&nameIdx, p, 4);
            uint8_t cls = p[4], n = p[5];
            const uint8_t *pts = p + 6;
            p = pts + n * 4;
            if (p > end) break;
            float runBest = 1e9f;
            int16_t v[2];
            memcpy(v, pts, 4);
            float ay = (v[0] - carY) * ky, ax = (v[1] - carX) * kx;
            for (uint8_t k = 1; k < n; k++) {
                memcpy(v, pts + k * 4, 4);
                float by = (v[0] - carY) * ky, bx = (v[1] - carX) * kx;
                float sx = bx - ax, sy = by - ay, len2 = sx * sx + sy * sy;
                float t = len2 > 0.01f ? -(ax * sx + ay * sy) / len2 : 0.0f;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                float px = ax + t * sx, py = ay + t * sy;
                float d = sqrtf(px * px + py * py);
                if (d <= kMaxSnapM) {
                    float score = d + cls * kClassPenaltyM;
                    if (useHeading && len2 > 1.0f) {
                        float diff = angleDiff180(atan2f(sx, sy) * (float)RAD_TO_DEG, headingDeg);
                        score += diff > 50.0f ? 40.0f : diff * 0.25f;
                    }
                    if (score < runBest) runBest = score;
                }
                ax = bx;
                ay = by;
            }
            if (runBest < bestScore) {
                bestScore = runBest;
                bestName = nameIdx;
            }
            if ((int64_t)nameIdx == prefer && runBest < curScore) curScore = runBest;
        }
    }
}

void readName(uint32_t idx) {
    gName[0] = '\0';
    uint32_t off = 0;
    if (idx >= gH.nameCount || !sdMgrReadBytes(gPath, gH.nameIndexOff + idx * 4, (uint8_t *)&off, 4)) return;
    uint8_t buf[sizeof(gName)];
    // The blob may end closer than sizeof(buf) — read what fits.
    size_t n = sizeof(buf) - 1;
    if (!sdMgrReadBytes(gPath, gH.nameBlobOff + off, buf, n)) {
        n = 32;
        if (!sdMgrReadBytes(gPath, gH.nameBlobOff + off, buf, n)) return;
    }
    buf[n] = 0;
    memcpy(gName, buf, n + 1);
}
} // namespace

void roadNamesReset() {
    gTried = gOk = false;
    gBlockR = gBlockC = -1;
    gCurrent = gCandidate = -1;
    gName[0] = '\0';
}

const char *roadNamesLookup(float lat, float lon, bool headingValid, float headingDeg, float speedKmh) {
    uint32_t now = millis();
    if (now - gLastQueryMs < kQueryIntervalMs) return gName;
    gLastQueryMs = now;
    if (!openFile()) return "";

    int32_t latE5 = (int32_t)lroundf(lat * 1e5f), lonE5 = (int32_t)lroundf(lon * 1e5f);
    int32_t r = (latE5 - gH.minLatE5) / (int32_t)gH.cellE5, c = (lonE5 - gH.minLonE5) / (int32_t)gH.cellE5;
    if (latE5 < gH.minLatE5 || lonE5 < gH.minLonE5 || r >= gH.rows || c >= gH.cols) {
        if (now - gLastOnRoadMs > kLostTimeoutMs) { gCurrent = -1; gName[0] = '\0'; }
        return gName;
    }
    if (r != gBlockR || c != gBlockC) loadBlock(r, c);

    int64_t best;
    float bestScore, curScore;
    search(latE5, lonE5, headingDeg, headingValid && speedKmh >= 8.0f, gCurrent, best, bestScore, curScore);
    if (best < 0) {
        if (now - gLastOnRoadMs > kLostTimeoutMs) { gCurrent = -1; gName[0] = '\0'; }
        return gName;
    }
    gLastOnRoadMs = now;
    int64_t next = gCurrent;
    if (gCurrent < 0 || best == gCurrent) {
        next = best;
        gCandidate = -1;
        gCandidateHits = 0;
    } else {
        // A different road scores best: switch only when it keeps winning
        // clearly, or quickly when the current road is no longer nearby.
        bool currentGone = curScore >= 1e8f;
        if (!currentGone && bestScore + kSwitchMarginM > curScore) {
            gCandidate = -1;
            gCandidateHits = 0;
        } else {
            if (best != gCandidate) { gCandidate = best; gCandidateHits = 0; }
            if (++gCandidateHits >= (currentGone ? 2 : kSwitchHits)) {
                next = gCandidate;
                gCandidate = -1;
                gCandidateHits = 0;
            }
        }
    }
    if (next != gCurrent || !gName[0]) {
        gCurrent = next;
        readName((uint32_t)gCurrent);
    }
    return gName;
}
