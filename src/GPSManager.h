#pragma once

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include "HardwareConfig.h"

class GPSManager {
public:
    GPSManager();
    void begin(uint32_t baud = GPS_BAUD_DEFAULT);
    void update();

    int getSpeedKmh();
    int getSatellites();
    bool hasFix();
    float getLatitude();
    float getLongitude();
    float getCourse();
    uint32_t getCharsProcessed();

private:
    TinyGPSPlus m_gps;
    HardwareSerial m_serial;
};
