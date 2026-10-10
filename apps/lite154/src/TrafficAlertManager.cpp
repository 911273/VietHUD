#include "TrafficAlertManager.h"
#include <math.h>

TrafficAlertManager trafficAlerts;

// Bounding box for Hanoi and 200km surrounding region (lat: 19.2 - 22.8, lon: 103.9 - 107.8)
#define HANOI_MIN_LAT_E7  192000000
#define HANOI_MAX_LAT_E7  228000000
#define HANOI_MIN_LON_E7  1039000000
#define HANOI_MAX_LON_E7  1078000000

TrafficAlertManager::TrafficAlertManager()
    : m_lastSoundAlertTime(0), m_lastSoundAlertType(0) {
}

void TrafficAlertManager::begin() {
    Serial.printf("[TRAFFIC] Da nap database canh bao Ha Noi & 200km: %d diem canh bao.\n", HANOI_ALERT_COUNT);
    Serial.println("[TRAFFIC] Du lieu bao gom: Camera toc do, Camera vuot den do, Khu dan cu, Cam vuot, Tram thu phi.");
}

ActiveTrafficAlert TrafficAlertManager::queryNearby(float lat, float lon, float headingDeg, float speedKmh) {
    ActiveTrafficAlert result;
    result.active = false;
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
        return result; // Outside Hanoi 200km region
    }

    // Search window: +/- 60,000 E7 (~660m lat range)
    int32_t minLat = targetLat - 60000;
    int32_t maxLat = targetLat + 60000;

    // Binary search for start index (since HANOI_ALERTS is sorted by latE7)
    int low = 0;
    int high = HANOI_ALERT_COUNT - 1;
    int startIdx = 0;

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

    float bestDist = 550.0f;
    int bestIdx = -1;

    // Iterate through the narrow latitude slice
    for (int i = startIdx; i < HANOI_ALERT_COUNT; i++) {
        int32_t aLat = pgm_read_dword(&(HANOI_ALERTS[i].latE7));
        if (aLat > maxLat) break; // Exceeded search band

        int32_t aLon = pgm_read_dword(&(HANOI_ALERTS[i].lonE7));
        int32_t dLon = abs(aLon - targetLon);
        if (dLon > 65000) continue; // Outside lon window

        // Calculate distance in meters
        float dy = (aLat - targetLat) * 0.0000111139f;
        float dx = (aLon - targetLon) * 0.0000103600f;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist <= bestDist) {
            uint16_t aHeading = pgm_read_word(&(HANOI_ALERTS[i].heading));
            // Check direction if specified
            if (aHeading > 0 && aHeading < 360 && speedKmh > 10.0f) {
                int diff = abs((int)aHeading - (int)headingDeg);
                if (diff > 180) diff = 360 - diff;
                if (diff > 65) continue; // Faces opposite direction
            }

            bestDist = dist;
            bestIdx = i;
        }
    }

    if (bestIdx >= 0) {
        result.active = true;
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
