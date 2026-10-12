#include "TrafficAlertManager.h"
#include <math.h>

TrafficAlertManager trafficAlerts;

// Bounding box for Hanoi and 200km surrounding region (lat: 19.2 - 22.8, lon: 103.9 - 107.8)
#define HANOI_MIN_LAT_E7  192000000
#define HANOI_MAX_LAT_E7  228000000
#define HANOI_MIN_LON_E7  1039000000
#define HANOI_MAX_LON_E7  1078000000

namespace {
const float kMaxRangeM = 550.0f;
const float kMinMovingKmh = 8.0f;     // GPS course is noise below this
const float kPassedM = 12.0f;         // closer than this along the road = passing it now -> drop
const float kHeadingTolDeg = 50.0f;   // point's facing vs car course
// Allowed sideways offset from the car's line of travel: tight close up (other carriageway,
// side street), looser far ahead where the road may curve.
const float kLateralBaseM = 25.0f;
const float kLateralPerM = 0.30f;
const float kMPerE7Lat = 0.0111139f;
const float kMPerE7Lon = 0.0103600f;  // at ~21 N
}

TrafficAlertManager::TrafficAlertManager() : m_latchedIdx(-1) {
}

void TrafficAlertManager::begin() {
    Serial.printf("[TRAFFIC] Da nap database canh bao Ha Noi & 200km: %d diem canh bao.\n", HANOI_ALERT_COUNT);
    Serial.println("[TRAFFIC] Du lieu bao gom: Camera toc do, Camera phat nguoi, Den do, Khu dan cu, Cam vuot, Tram thu phi.");
}

// Is record idx a valid alert for a car at target heading headingDeg? Returns its distance.
bool TrafficAlertManager::evaluate(int idx, int32_t targetLat, int32_t targetLon, float headingDeg, bool moving,
                                   float &distOut) const {
    int32_t aLat = pgm_read_dword(&(HANOI_ALERTS[idx].latE7));
    int32_t aLon = pgm_read_dword(&(HANOI_ALERTS[idx].lonE7));
    float dy = (aLat - targetLat) * kMPerE7Lat;   // north, metres
    float dx = (aLon - targetLon) * kMPerE7Lon;   // east, metres
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > kMaxRangeM) return false;

    float h = headingDeg * (float)DEG_TO_RAD;
    float sh = sinf(h), ch = cosf(h);
    float along = dx * sh + dy * ch;              // ahead (+) / behind (-)
    float lateral = fabsf(dx * ch - dy * sh);
    if (along < kPassedM) return false;

    uint16_t aHeading = pgm_read_word(&(HANOI_ALERTS[idx].heading));
    bool hasHeading = aHeading > 0 && aHeading < 360;
    float maxLateral = kLateralBaseM + along * kLateralPerM;
    if (hasHeading) {
        int diff = abs((int)aHeading - (int)headingDeg);
        if (diff > 180) diff = 360 - diff;
        if (moving && diff > kHeadingTolDeg) return false;   // faces the other carriageway
    } else {
        maxLateral *= 0.6f;                                    // direction unknown: be stricter
    }
    if (lateral > maxLateral) return false;

    distOut = dist;
    return true;
}

ActiveTrafficAlert TrafficAlertManager::queryNearby(float lat, float lon, float headingDeg, float speedKmh) {
    ActiveTrafficAlert result;
    result.active = false;
    result.index = -1;
    result.type = ALERT_NONE;
    result.speedLimit = 0;
    result.distanceM = 0;
    result.label[0] = '\0';
    result.icon[0] = '\0';
    result.color = 0xFFFF;

    int32_t targetLat = (int32_t)round(lat * 1e7);
    int32_t targetLon = (int32_t)round(lon * 1e7);

    // Range check
    if (targetLat < HANOI_MIN_LAT_E7 || targetLat > HANOI_MAX_LAT_E7 ||
        targetLon < HANOI_MIN_LON_E7 || targetLon > HANOI_MAX_LON_E7) {
        m_latchedIdx = -1;
        return result; // Outside Hanoi 200km region
    }

    bool moving = speedKmh >= kMinMovingKmh;
    float bestDist = kMaxRangeM + 1.0f;
    int bestIdx = -1;

    if (moving) {
        // Search window: +/- 60,000 E7 (~660m lat range)
        int32_t minLat = targetLat - 60000;
        int32_t maxLat = targetLat + 60000;

        // Binary search for start index (since HANOI_ALERTS is sorted by latE7)
        int low = 0;
        int high = HANOI_ALERT_COUNT - 1;
        int startIdx = HANOI_ALERT_COUNT;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            int32_t midLat = pgm_read_dword(&(HANOI_ALERTS[mid].latE7));
            if (midLat >= minLat) {
                startIdx = mid;
                high = mid - 1;
            } else {
                low = mid + 1;
            }
        }

        for (int i = startIdx; i < HANOI_ALERT_COUNT; i++) {
            int32_t aLat = pgm_read_dword(&(HANOI_ALERTS[i].latE7));
            if (aLat > maxLat) break; // Exceeded search band
            int32_t aLon = pgm_read_dword(&(HANOI_ALERTS[i].lonE7));
            if (abs(aLon - targetLon) > 65000) continue; // Outside lon window

            float dist;
            if (evaluate(i, targetLat, targetLon, headingDeg, true, dist) && dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
            }
        }
    } else if (m_latchedIdx >= 0) {
        // Slow / stopped: keep showing the alert we were approaching, nothing new.
        float dist;
        if (evaluate(m_latchedIdx, targetLat, targetLon, headingDeg, false, dist)) {
            bestDist = dist;
            bestIdx = m_latchedIdx;
        }
    }

    m_latchedIdx = bestIdx;
    if (bestIdx >= 0) {
        result.active = true;
        result.index = bestIdx;
        result.type = pgm_read_byte(&(HANOI_ALERTS[bestIdx].type));
        result.speedLimit = pgm_read_byte(&(HANOI_ALERTS[bestIdx].speed));
        result.distanceM = (uint16_t)round(bestDist);

        switch (result.type) {
            case ALERT_SPEED_CAMERA:
                strncpy(result.icon, "CAM", sizeof(result.icon));
                if (result.speedLimit > 0) {
                    snprintf(result.label, sizeof(result.label), "CAM TOC DO: %d km/h", result.speedLimit);
                } else {
                    strncpy(result.label, "CAMERA BAN TOC DO", sizeof(result.label));
                }
                result.color = 0xF800; // Red
                break;

            case ALERT_CAMERA:
                strncpy(result.icon, "CAM", sizeof(result.icon));
                strncpy(result.label, "CAMERA PHAT NGUOI", sizeof(result.label));
                result.color = 0xF800; // Red
                break;

            case ALERT_TRAFFIC_LIGHT:
                strncpy(result.icon, "RED", sizeof(result.icon));
                strncpy(result.label, "CAM VUOT DEN DO", sizeof(result.label));
                result.color = 0xFD20; // Amber
                break;

            case ALERT_RESIDENT_AREA:
                strncpy(result.icon, "RES", sizeof(result.icon));
                strncpy(result.label, "KHU DONG DAN CU", sizeof(result.label));
                result.color = 0x07E0; // Emerald Green
                break;

            case ALERT_NO_OVERTAKING:
                strncpy(result.icon, "NOV", sizeof(result.icon));
                strncpy(result.label, "DOAN DUONG CAM VUOT", sizeof(result.label));
                result.color = 0xFD20; // Amber
                break;

            case ALERT_TOLL_BOOTH:
                strncpy(result.icon, "BOT", sizeof(result.icon));
                strncpy(result.label, "TRAM THU PHI BOT", sizeof(result.label));
                result.color = 0x07FF; // Cyan
                break;

            case ALERT_DANGER:
            default:
                strncpy(result.icon, "DNG", sizeof(result.icon));
                strncpy(result.label, "CANH BAO NGUY HIEM", sizeof(result.label));
                result.color = 0xFD20; // Amber
                break;
        }
    }

    return result;
}
