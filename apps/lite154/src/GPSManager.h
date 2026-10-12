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
//  - speed = module Doppler velocity averaged over 3 samples (~0.3 s), with
//    start/stop hysteresis so it reads exactly 0 while parked.
// On the first UBX frame the module is set to the automotive dynamic model with
// static hold, and given the last saved position to speed up the first fix.
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
    bool rawVelLog = false;          // serial "VEL=1": one line per NAV-PVT for filter tuning
    uint32_t getTtffMs() const { return m_ttffMs; }   // boot -> first usable position, 0 = not yet
    uint32_t getGlitchCount() const { return m_glitchCount; }

private:
    void openUart(uint32_t baud);
    bool feedUbx(uint8_t c);
    void onNavPvt(const uint8_t *p);
    void autoBaudStep(uint32_t now);
    bool ubxFresh(uint32_t now) const;
    bool velFresh(uint32_t now) const;
    void sendUbx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len);
    void configureModule();
    void saveLastPosition();

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
    uint32_t m_velMs = 0;         // last NAV-PVT with a valid fix (velocity usable even if hAcc is poor)
    bool m_moving = false;
    uint8_t m_moveHits = 0;
    int32_t m_heightCm = 0;
    uint32_t m_ttffMs = 0;

    // Glitch rejection state (onNavPvt)
    float m_histN[2] = {0, 0}, m_histE[2] = {0, 0};   // last two raw velocities
    uint32_t m_histMs[2] = {0, 0};
    uint8_t m_histCount = 0;
    float m_goodN = 0, m_goodE = 0;                   // last trusted velocity
    uint32_t m_goodMs = 0;
    bool m_hasGood = false;
    bool m_glitch = false;
    uint32_t m_glitchMs = 0;
    uint32_t m_glitchCount = 0;
    uint8_t m_stable = 0;

    // Module setup + last-position aiding
    bool m_configured = false;
    uint8_t m_cfgSent = 0, m_cfgAcked = 0, m_cfgNaked = 0;
    int32_t m_savedLatE7 = 0, m_savedLonE7 = 0;
    bool m_hasSavedPos = false;
};
