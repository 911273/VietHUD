#include "GPSManager.h"

GPSManager::GPSManager() : m_serial(1) {}

void GPSManager::begin(uint32_t baud) {
    // UART1 with RX=GPIO44, TX=GPIO43
    m_serial.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
}

void GPSManager::update() {
    while (m_serial.available() > 0) {
        char c = m_serial.read();
        m_gps.encode(c);
    }
}

int GPSManager::getSpeedKmh() {
    if (m_gps.speed.isValid()) {
        return (int)round(m_gps.speed.kmph());
    }
    return 0;
}

int GPSManager::getSatellites() {
    if (m_gps.satellites.isValid()) {
        return m_gps.satellites.value();
    }
    return 0;
}

bool GPSManager::hasFix() {
    return m_gps.location.isValid() && m_gps.satellites.isValid() && m_gps.satellites.value() >= 3;
}

float GPSManager::getLatitude() {
    return m_gps.location.isValid() ? (float)m_gps.location.lat() : 0.0f;
}

float GPSManager::getLongitude() {
    return m_gps.location.isValid() ? (float)m_gps.location.lng() : 0.0f;
}

float GPSManager::getCourse() {
    return m_gps.course.isValid() ? (float)m_gps.course.deg() : 0.0f;
}

uint32_t GPSManager::getCharsProcessed() {
    return m_gps.charsProcessed();
}
