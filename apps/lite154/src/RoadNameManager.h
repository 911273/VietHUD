#pragma once

#include <Arduino.h>

// Live road name from the embedded OSM road database (data/roads_hn.bin, built by
// tools/build_road_db.py). Snaps the GPS position to the nearest named road segment,
// preferring roads that run along the car's heading and more important road classes,
// with hysteresis so the name does not flicker at junctions or beside parallel streets.
class RoadNameManager {
public:
    bool begin();

    // Call on every GPS update; internally rate-limited.
    void update(float lat, float lon, float headingDeg, int speedKmh, uint32_t nowMs);
    void clear();

    // Current road name (UTF-8), or "" when not on a known road.
    const char *name() const;
    bool isReady() const { return m_ok; }
    uint32_t nameCount() const { return m_nameCount; }

private:
    struct Match {
        int nameIdx = -1;
        float score = 1e9f;
        float dist = 1e9f;
    };

    void search(int32_t latE5, int32_t lonE5, float headingDeg, bool useHeading, int preferName,
                Match &best, float &curScore) const;
    const uint8_t *findCell(uint32_t cellId) const;
    const char *nameAt(int idx) const;

    bool m_ok = false;
    const uint8_t *m_base = nullptr;
    uint32_t m_cellCount = 0, m_nameCount = 0;
    uint32_t m_cellIndexOff = 0, m_nameIndexOff = 0, m_nameBlobOff = 0;
    int32_t m_minLatE5 = 0, m_minLonE5 = 0;
    uint16_t m_cellE5 = 0, m_rows = 0, m_cols = 0;

    int m_current = -1;          // shown name index
    int m_candidate = -1;        // challenger waiting to replace it
    uint8_t m_candidateHits = 0;
    uint32_t m_lastQueryMs = 0;
    uint32_t m_lastOnRoadMs = 0;
};

extern RoadNameManager roadNames;
