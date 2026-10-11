#include "GPSManager.h"
#include <math.h>

// Bauds tried in order until valid data shows up. P18 Pro first (115200).
static const uint32_t kBauds[] = {115200, 9600, 38400, 57600};
static const int kBaudCount = sizeof(kBauds) / sizeof(kBauds[0]);
static const uint32_t kBaudTryMs = 2500;      // per candidate while nothing valid arrives
static const uint32_t kLinkTimeoutMs = 3000;
static const uint32_t kFixTimeoutMs = 2000;
static const float kMaxHAccM = 50.0f;         // worse than this = not a usable position
static const float kStaticHoldKmh = 5.0f;
static const int kRxChunk = 1024;

static inline int32_t rdI32(const uint8_t *p, int o) {
    return (int32_t)((uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) |
                     ((uint32_t)p[o + 3] << 24));
}

GPSManager::GPSManager() : m_serial(1) {}

void GPSManager::openUart(uint32_t baud) {
    m_serial.end();
    m_serial.setRxBufferSize(4096); // must precede begin()
    m_serial.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    m_baud = baud;
    m_baudSince = millis();
    m_ubxState = 0;
    Serial.printf("[GPS] listening at %lu baud\n", (unsigned long)baud);
}

void GPSManager::begin(uint32_t baud) {
    // UART1 with RX=GPIO44, TX=GPIO43. Start at the requested baud if it is a
    // candidate; autoBaudStep() walks the list until data is recognised.
    m_baudIdx = 0;
    for (int i = 0; i < kBaudCount; i++)
        if (kBauds[i] == baud) m_baudIdx = i;
    openUart(kBauds[m_baudIdx]);
}

// Minimal UBX frame receiver. Returns true if the byte belonged to a UBX frame
// (0xB5 never appears in NMEA text, so both streams can share the UART).
bool GPSManager::feedUbx(uint8_t c) {
    switch (m_ubxState) {
    case 0:
        if (c == 0xB5) { m_ubxState = 1; return true; }
        return false;
    case 1:
        if (c == 0x62) { m_ubxState = 2; m_ckA = m_ckB = 0; return true; }
        m_ubxState = 0;
        return false;
    case 2: m_cls = c; m_ckA += c; m_ckB += m_ckA; m_ubxState = 3; return true;
    case 3: m_id = c; m_ckA += c; m_ckB += m_ckA; m_ubxState = 4; return true;
    case 4: m_len = c; m_ckA += c; m_ckB += m_ckA; m_ubxState = 5; return true;
    case 5:
        m_len |= (uint16_t)c << 8;
        m_ckA += c; m_ckB += m_ckA;
        m_got = 0;
        if (m_len > 1024) { m_ubxState = 0; return true; } // garbage length
        m_ubxState = m_len ? 6 : 7;
        return true;
    case 6:
        if (m_got < sizeof(m_buf)) m_buf[m_got] = c;
        m_got++;
        m_ckA += c; m_ckB += m_ckA;
        if (m_got >= m_len) m_ubxState = 7;
        return true;
    case 7:
        m_ubxState = (c == m_ckA) ? 8 : 0;
        if (!m_ubxState) m_ubxBad++;
        return true;
    case 8:
        m_ubxState = 0;
        if (c != m_ckB) { m_ubxBad++; return true; }
        m_lastValidMs = millis();
        m_ubxFrames++;
        if (m_cls == 0x01 && m_id == 0x07 && m_len >= 92) onNavPvt(m_buf); // NAV-PVT
        return true;
    }
    m_ubxState = 0;
    return false;
}

void GPSManager::onNavPvt(const uint8_t *p) {
    uint8_t fixType = p[20], flags = p[21];
    m_numSv = p[23];
    m_hAccM = (uint32_t)rdI32(p, 40) / 1000.0f;
    float sAccMs = (uint32_t)rdI32(p, 68) / 1000.0f;
    bool fixOk = (flags & 0x01) && fixType >= 2 && fixType <= 4;
    if (!fixOk) {
        m_vN = 0;
        return;
    }
    // Speed: Doppler velocity averaged over the last 3 samples (~0.3 s at 10 Hz).
    m_vn[m_vIdx] = rdI32(p, 48) / 1000.0f;
    m_ve[m_vIdx] = rdI32(p, 52) / 1000.0f;
    m_vIdx = (m_vIdx + 1) % 3;
    if (m_vN < 3) m_vN++;
    float sn = 0, se = 0;
    for (int i = 0; i < m_vN; i++) { sn += m_vn[i]; se += m_ve[i]; }
    sn /= m_vN;
    se /= m_vN;
    float ms = sqrtf(sn * sn + se * se);
    if (ms * 3.6f < kStaticHoldKmh && ms < sAccMs) ms = 0; // stopped: hold 0, no jitter
    m_speedKmh = ms * 3.6f;
    // Course only means something while moving; keep the last one when stopped.
    if (m_speedKmh >= 3.0f) {
        float c = atan2f(se, sn) * 180.0f / (float)M_PI;
        m_courseDeg = c < 0 ? c + 360.0f : c;
    }
    if (m_hAccM <= kMaxHAccM) {
        m_lon = rdI32(p, 24) / 1e7;
        m_lat = rdI32(p, 28) / 1e7;
        m_pvtMs = millis();
    }
}

bool GPSManager::ubxFresh(uint32_t now) const { return m_pvtMs != 0 && now - m_pvtMs < kFixTimeoutMs; }

void GPSManager::autoBaudStep(uint32_t now) {
    uint32_t passed = m_gps.passedChecksum(); // NMEA sentences count as valid data too
    if (passed != m_lastPassed) {
        m_lastPassed = passed;
        m_lastValidMs = now;
    }
    bool seenSinceOpen = m_lastValidMs != 0 && (int32_t)(m_lastValidMs - m_baudSince) >= 0;
    if (!seenSinceOpen && now - m_baudSince > kBaudTryMs) {
        m_baudIdx = (m_baudIdx + 1) % kBaudCount;
        openUart(kBauds[m_baudIdx]);
    } else if (seenSinceOpen && now - m_lastValidMs > 10000) {
        // Was talking, then silent for 10 s (module unplugged/rebooted at
        // another baud): start hunting again from this baud.
        m_baudSince = now;
        m_lastValidMs = 0;
    }
    if (now - m_lastLogMs > 10000) {
        m_lastLogMs = now;
        Serial.printf("[GPS] %s @%lu  chars=%lu ubx=%lu bad=%lu nmea=%lu  fix=%d sats=%d hAcc=%.1fm spd=%d\n",
                      getProtocol(), (unsigned long)m_baud, (unsigned long)m_chars,
                      (unsigned long)m_ubxFrames, (unsigned long)m_ubxBad, (unsigned long)passed, hasFix(), getSatellites(),
                      (double)m_hAccM, getSpeedKmh());
    }
}

void GPSManager::update() {
    // Bounded bulk read: one driver call for at most kRxChunk bytes, so a
    // module streaming continuously can never keep this from returning.
    static uint8_t rx[kRxChunk];
    int avail = m_serial.available();
    if (avail > 0) {
        int n = m_serial.readBytes(rx, avail < kRxChunk ? avail : kRxChunk);
        m_chars += n;
        for (int i = 0; i < n; i++)
            if (!feedUbx(rx[i])) m_gps.encode((char)rx[i]);
    }
    autoBaudStep(millis());
}

int GPSManager::getSpeedKmh() {
    if (ubxFresh(millis())) return (int)lroundf(m_speedKmh);
    if (m_gps.speed.isValid() && m_gps.speed.age() < kFixTimeoutMs) return (int)lround(m_gps.speed.kmph());
    return 0;
}

int GPSManager::getSatellites() {
    if (m_ubxFrames) return m_numSv;
    return m_gps.satellites.isValid() ? (int)m_gps.satellites.value() : 0;
}

bool GPSManager::hasFix() {
    if (ubxFresh(millis())) return true;
    return m_gps.location.isValid() && m_gps.location.age() < kFixTimeoutMs && m_gps.satellites.isValid() &&
           m_gps.satellites.value() >= 3;
}

float GPSManager::getLatitude() {
    if (ubxFresh(millis())) return (float)m_lat;
    return m_gps.location.isValid() ? (float)m_gps.location.lat() : 0.0f;
}

float GPSManager::getLongitude() {
    if (ubxFresh(millis())) return (float)m_lon;
    return m_gps.location.isValid() ? (float)m_gps.location.lng() : 0.0f;
}

float GPSManager::getCourse() {
    if (ubxFresh(millis())) return m_courseDeg;
    return m_gps.course.isValid() ? (float)m_gps.course.deg() : 0.0f;
}

uint32_t GPSManager::getCharsProcessed() { return m_chars; }

bool GPSManager::isLinkAlive() { return m_lastValidMs != 0 && millis() - m_lastValidMs < kLinkTimeoutMs; }

const char *GPSManager::getProtocol() {
    if (m_ubxFrames) return "UBX";
    return m_gps.passedChecksum() ? "NMEA" : "--";
}
