#pragma once

#include <Arduino.h>
#include "HanoiAlertsData.h"

enum TrafficAlertType : uint8_t {
    ALERT_NONE           = 0,
    ALERT_SPEED_CAMERA   = 1, // Camera bắn tốc độ / P.127: Giới hạn tốc độ
    ALERT_RESIDENT_AREA  = 2, // R.420 / R.421: Khu đông dân cư
    ALERT_NO_OVERTAKING  = 3, // P.125 / DP.133: Đoạn đường cấm vượt
    ALERT_CAMERA         = 4, // Camera phạt nguội
    ALERT_TOLL_BOOTH     = 5, // P.135: Trạm thu phí BOT
    ALERT_TRAFFIC_LIGHT  = 6, // Đèn tín hiệu giao thông / Camera vượt đèn đỏ
    ALERT_DANGER         = 10 // Cảnh báo nguy hiểm / Hầm / Cầu
};

struct ActiveTrafficAlert {
    bool active;
    uint8_t type;        // TrafficAlertType
    uint8_t speedLimit;  // km/h (or 0)
    uint16_t distanceM;  // Distance in meters
    char label[36];      // Clean label
    char icon[8];        // Badge identifier: "CAM", "RED", "RES", "BOT", "NOV", "DNG"
    uint16_t color;      // Display color
};

class TrafficAlertManager {
public:
    TrafficAlertManager();
    void begin();

    // Query active nearby alerts based on current GPS position
    ActiveTrafficAlert queryNearby(float lat, float lon, float headingDeg, float speedKmh);

    // Get count of alerts in the database
    uint32_t getAlertCount() const { return HANOI_ALERT_COUNT; }

private:
    uint32_t m_lastSoundAlertTime;
    uint8_t  m_lastSoundAlertType;
};

extern TrafficAlertManager trafficAlerts;
