#pragma once

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include "HardwareConfig.h"

// GPS input for VietHUD Lite. Understands both kinds of module it may be fed:
//  - "P18 Pro" / u-blox M10 drone modules that send binary UBX NAV-PVT only
//    (115200 baud, ~10 Hz, NMEA switched off), and
//  - classic NMEA modules (BN-220, NEO-6M...) via TinyGPSPlus.
// The baud rate is found automatically, so either type works as plugged in.
// Ported from VietHUD 2.8's GNSS code (radar_car src/gnss/GNSS.cpp), with its fixes:
//  - 4 KB UART RX buffer + bounded bulk reads: with the 256 B default the
//    10 Hz UBX stream overflowed and the driver stalled the loop (watchdog reboot);
//  - a fix goes stale after 2 s without a fresh position, so losing the sky
//    (tunnel) no longer freezes the last speed/position on screen;
//  - speed = module Doppler velocity averaged over 3 samples (~0.3 s), held
//    at 0 when stopped (below the module's own speed accuracy).
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

    // Diagnostics
    bool isLinkAlive();              // valid UBX frame / NMEA sentence in the last 3 s
    uint32_t getBaud() const { return m_baud; }
    const char *getProtocol();       // "UBX", "NMEA" or "--"
    float getHAccM() const { return m_hAccM; }

private:
    void openUart(uint32_t baud);
    bool feedUbx(uint8_t c);
    void onNavPvt(const uint8_t *p);
    void autoBaudStep(uint32_t now);
    bool ubxFresh(uint32_t now) const;

    TinyGPSPlus m_gps;
    HardwareSerial m_serial;
    uint32_t m_baud = 0;
    int m_baudIdx = 0;
    uint32_t m_baudSince = 0;
    uint32_t m_lastValidMs = 0;   // last valid UBX frame / NMEA sentence
    uint32_t m_lastPassed = 0;    // TinyGPS passedChecksum() last seen
    uint32_t m_ubxFrames = 0;
    uint32_t m_ubxBad = 0;        // UBX frames with a bad checksum
    uint32_t m_chars = 0;
    uint32_t m_lastLogMs = 0;

    // UBX frame receiver
    uint8_t m_ubxState = 0, m_cls = 0, m_id = 0, m_ckA = 0, m_ckB = 0;
    uint16_t m_len = 0, m_got = 0;
    uint8_t m_buf[100];

    // Latest NAV-PVT
    uint32_t m_pvtMs = 0;         // last NAV-PVT with a usable fix
    double m_lat = 0, m_lon = 0;
    float m_speedKmh = 0, m_courseDeg = 0, m_hAccM = 0;
    int m_numSv = 0;
    float m_vn[3] = {0, 0, 0}, m_ve[3] = {0, 0, 0};
    int m_vN = 0, m_vIdx = 0;
};
