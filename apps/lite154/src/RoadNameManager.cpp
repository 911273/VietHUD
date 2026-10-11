#include "RoadNameManager.h"
#include <math.h>
#include <string.h>

RoadNameManager roadNames;

// data/roads_hn.bin, embedded by board_build.embed_files in platformio.ini
extern const uint8_t roads_bin_start[] asm("_binary_data_roads_hn_bin_start");
extern const uint8_t roads_bin_end[] asm("_binary_data_roads_hn_bin_end");

namespace {
const uint32_t kQueryIntervalMs = 700;
const uint32_t kLostTimeoutMs = 8000;   // keep the last name this long off-road (tunnels, GPS drift)
const float kMaxSnapM = 30.0f;          // farther than this from any road = not on a known road
const float kClassPenaltyM = 1.2f;      // per class step: motorway 0 ... residential 6, links 7
const float kSwitchMarginM = 6.0f;      // challenger must beat the current road by this much ...
const uint8_t kSwitchHits = 3;          // ... this many queries in a row (~2 s)
const float kMetersPerE5Lat = 1.11195f;

template <typename T> T rd(const uint8_t *p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
}

float angleDiff180(float a, float b) {
    float d = fabsf(fmodf(a - b, 180.0f));
    return d > 90.0f ? 180.0f - d : d;
}
} // namespace

bool RoadNameManager::begin() {
    const uint8_t *p = roads_bin_start;
    size_t len = roads_bin_end - roads_bin_start;
    if (len < 40 || memcmp(p, "VHRD", 4) != 0 || rd<uint16_t>(p + 4) != 1) {
        Serial.println("[ROAD] Khong tim thay du lieu ten duong (roads_hn.bin).");
        return false;
    }
    m_base = p;
    m_cellE5 = rd<uint16_t>(p + 6);
    m_minLatE5 = rd<int32_t>(p + 8);
    m_minLonE5 = rd<int32_t>(p + 12);
    m_rows = rd<uint16_t>(p + 16);
    m_cols = rd<uint16_t>(p + 18);
    m_cellCount = rd<uint32_t>(p + 20);
    m_nameCount = rd<uint32_t>(p + 24);
    m_cellIndexOff = rd<uint32_t>(p + 28);
    m_nameIndexOff = rd<uint32_t>(p + 32);
    m_nameBlobOff = rd<uint32_t>(p + 36);
    m_ok = true;
    Serial.printf("[ROAD] Da nap %u ten duong, %u o luoi (%u KB).\n",
                  (unsigned)m_nameCount, (unsigned)m_cellCount, (unsigned)(len / 1024));
    return true;
}

const uint8_t *RoadNameManager::findCell(uint32_t cellId) const {
    const uint8_t *idx = m_base + m_cellIndexOff;
    int lo = 0, hi = (int)m_cellCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t id = rd<uint32_t>(idx + mid * 8);
        if (id == cellId) return m_base + rd<uint32_t>(idx + mid * 8 + 4);
        if (id < cellId) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

const char *RoadNameManager::nameAt(int idx) const {
    if (!m_ok || idx < 0 || (uint32_t)idx >= m_nameCount) return "";
    return (const char *)(m_base + m_nameBlobOff + rd<uint32_t>(m_base + m_nameIndexOff + idx * 4));
}

const char *RoadNameManager::name() const {
    return nameAt(m_current);
}

void RoadNameManager::clear() {
    m_current = -1;
    m_candidate = -1;
    m_candidateHits = 0;
}

// Best-scoring road overall into `best`; `curScore` gets the best score of road `preferName`
// (the one currently shown), or 1e9 when it is not nearby.
void RoadNameManager::search(int32_t latE5, int32_t lonE5, float headingDeg, bool useHeading, int preferName,
                             Match &best, float &curScore) const {
    best = Match();
    curScore = 1e9f;
    const float ky = kMetersPerE5Lat;
    const float kx = kMetersPerE5Lat * cosf(latE5 * 1e-5f * (float)DEG_TO_RAD);

    int row = (latE5 - m_minLatE5) / m_cellE5;
    int col = (lonE5 - m_minLonE5) / m_cellE5;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int r = row + dr, c = col + dc;
            if (r < 0 || c < 0 || r >= m_rows || c >= m_cols) continue;
            const uint8_t *p = findCell((uint32_t)r * m_cols + c);
            if (!p) continue;
            // Car position relative to this cell's origin, in 1e-5 deg
            float carY = (float)(latE5 - (m_minLatE5 + r * m_cellE5));
            float carX = (float)(lonE5 - (m_minLonE5 + c * m_cellE5));

            uint16_t runs = rd<uint16_t>(p);
            p += 2;
            for (uint16_t i = 0; i < runs; i++) {
                uint16_t nameIdx = rd<uint16_t>(p);
                uint8_t cls = p[2];
                uint8_t n = p[3];
                const uint8_t *pts = p + 4;
                p = pts + n * 4;

                float runBest = 1e9f, runDist = 1e9f;
                float ax = (rd<int16_t>(pts + 2) - carX) * kx;
                float ay = (rd<int16_t>(pts) - carY) * ky;
                for (uint8_t k = 1; k < n; k++) {
                    float bx = (rd<int16_t>(pts + k * 4 + 2) - carX) * kx;
                    float by = (rd<int16_t>(pts + k * 4) - carY) * ky;
                    float sx = bx - ax, sy = by - ay;
                    float len2 = sx * sx + sy * sy;
                    float t = len2 > 0.01f ? -(ax * sx + ay * sy) / len2 : 0.0f;
                    if (t < 0) t = 0; else if (t > 1) t = 1;
                    float px = ax + t * sx, py = ay + t * sy;
                    float d = sqrtf(px * px + py * py);
                    if (d <= kMaxSnapM && d < runDist + 40.0f) {
                        float score = d + cls * kClassPenaltyM;
                        if (useHeading && len2 > 1.0f) {
                            float segBearing = atan2f(sx, sy) * (float)RAD_TO_DEG;
                            float diff = angleDiff180(segBearing, headingDeg);
                            score += diff > 50.0f ? 40.0f : diff * 0.25f;
                        }
                        if (score < runBest) { runBest = score; runDist = d; }
                    }
                    ax = bx;
                    ay = by;
                }
                if (runBest < best.score) { best.score = runBest; best.dist = runDist; best.nameIdx = nameIdx; }
                if (nameIdx == preferName && runBest < curScore) curScore = runBest;
            }
        }
    }
}

void RoadNameManager::update(float lat, float lon, float headingDeg, int speedKmh, uint32_t nowMs) {
    if (!m_ok || nowMs - m_lastQueryMs < kQueryIntervalMs) return;
    m_lastQueryMs = nowMs;

    int32_t latE5 = (int32_t)lroundf(lat * 1e5f);
    int32_t lonE5 = (int32_t)lroundf(lon * 1e5f);
    Match best;
    float curScore;
    search(latE5, lonE5, headingDeg, speedKmh >= 8, m_current, best, curScore);

    if (best.nameIdx < 0) {
        if (nowMs - m_lastOnRoadMs > kLostTimeoutMs) clear();
        return;
    }
    m_lastOnRoadMs = nowMs;

    if (m_current < 0 || best.nameIdx == m_current) {
        m_current = best.nameIdx;
        m_candidate = -1;
        m_candidateHits = 0;
        return;
    }

    // A different road scores best. Switch only when it keeps winning clearly,
    // or right away-ish when the current road is no longer nearby at all.
    bool currentGone = curScore >= 1e8f;
    if (!currentGone && best.score + kSwitchMarginM > curScore) {
        m_candidate = -1;
        m_candidateHits = 0;
        return;
    }
    if (best.nameIdx != m_candidate) {
        m_candidate = best.nameIdx;
        m_candidateHits = 0;
    }
    if (++m_candidateHits >= (currentGone ? 2 : kSwitchHits)) {
        m_current = m_candidate;
        m_candidate = -1;
        m_candidateHits = 0;
    }
}
