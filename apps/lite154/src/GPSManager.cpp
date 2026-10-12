#include "GPSManager.h"
#include <Preferences.h>
#include <math.h>

// Bauds tried in order until valid data shows up. P18 Pro first (115200).
static const uint32_t kBauds[] = {115200, 9600, 38400, 57600};
static const int kBaudCount = sizeof(kBauds) / sizeof(kBauds[0]);
static const uint32_t kBaudTryMs = 2500;      // per candidate while nothing valid arrives
static const uint32_t kLinkTimeoutMs = 3000;
static const uint32_t kFixTimeoutMs = 2000;
static const float kMaxHAccM = 50.0f;         // worse than this = not a usable position
static const float kStartMovingKmh = 10.0f;  // must stay above this for kStartHits samples (1 s)
static const uint8_t kStartHits = 10;
static const float kStopMovingKmh = 5.0f;    // below this = parked, speed reads exactly 0
// Glitch rejection (see onNavPvt); values tuned on recorded P18 Pro data
static const float kJumpMs = 3.0f;           // velocity change within ~0.2 s that no car can make
static const float kStableDeltaMs = 1.0f;    // sample-to-sample change counted as "settled"
static const uint8_t kStableSamples = 5;
static const float kSlackMs = 1.5f;
static const float kMaxAccelMs2 = 4.0f;
static const float kMaxDecelMs2 = 10.0f;
static const uint32_t kGlitchMaxMs = 8000;   // give up and trust the module again after this
static const float kSavePosMaxHAccM = 20.0f;
static const float kSavePosMinMoveM = 200.0f;
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

void GPSManager::sendUbx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
    uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += hdr[i]; b += a; }
    for (uint16_t i = 0; i < len; i++) { a += payload[i]; b += a; }
    m_serial.write(hdr, 6);
    if (len) m_serial.write(payload, len);
    uint8_t ck[2] = {a, b};
    m_serial.write(ck, 2);
}

// u-blox M10 setup, sent once when the module is first heard (UBX = it is a u-blox).
// One key per CFG-VALSET: an unknown key makes the receiver reject the whole message.
// Layers RAM + BBR, so it also survives a power cut if the module has a backup cell.
void GPSManager::configureModule() {
    struct { uint32_t key; uint8_t size; uint32_t value; } cfg[] = {
        {0x20110021, 1, 4},    // CFG-NAVSPG-DYNMODEL = automotive
        {0x20250038, 1, 80},   // CFG-MOT-GNSSSPEED_THRS = 80 cm/s (static hold below ~2.9 km/h)
        {0x3025003B, 2, 5},    // CFG-MOT-GNSSDIST_THRS = 5 m (leave static hold after moving 5 m)
    };
    for (auto &c : cfg) {
        uint8_t p[4 + 4 + 4] = {0x00, 0x03, 0x00, 0x00};
        memcpy(p + 4, &c.key, 4);
        memcpy(p + 8, &c.value, c.size);
        sendUbx(0x06, 0x8A, p, 8 + c.size);
        m_cfgSent++;
    }

    // Last known position (UBX-MGA-INI-POS_LLH, 5 km accuracy) -> faster first fix
    Preferences prefs;
    if (prefs.begin("gps", true)) {
        m_hasSavedPos = prefs.isKey("lat");
        m_savedLatE7 = prefs.getInt("lat", 0);
        m_savedLonE7 = prefs.getInt("lon", 0);
        int32_t hCm = prefs.getInt("h", 0);
        prefs.end();
        if (m_hasSavedPos) {
            uint8_t p[20] = {0x01, 0x00, 0x00, 0x00};
            uint32_t accCm = 500000;
            memcpy(p + 4, &m_savedLatE7, 4);
            memcpy(p + 8, &m_savedLonE7, 4);
            memcpy(p + 12, &hCm, 4);
            memcpy(p + 16, &accCm, 4);
            sendUbx(0x13, 0x40, p, sizeof(p));
            Serial.printf("[GPS] Gui vi tri cuoi %.5f, %.5f cho module\n", m_savedLatE7 / 1e7, m_savedLonE7 / 1e7);
        }
    }
    m_configured = true;
    Serial.printf("[GPS] Da gui cau hinh u-blox: automotive + static hold (%u lenh)\n", m_cfgSent);
}

void GPSManager::saveLastPosition() {
    int32_t latE7 = (int32_t)lround(m_lat * 1e7), lonE7 = (int32_t)lround(m_lon * 1e7);
    if (m_hasSavedPos) {
        float dy = (latE7 - m_savedLatE7) * 0.0111139f;
        float dx = (lonE7 - m_savedLonE7) * 0.0103600f;
        if (dx * dx + dy * dy < kSavePosMinMoveM * kSavePosMinMoveM) return;
    }
    Preferences prefs;
    if (!prefs.begin("gps", false)) return;
    prefs.putInt("lat", latE7);
    prefs.putInt("lon", lonE7);
    prefs.putInt("h", m_heightCm);
    prefs.end();
    m_savedLatE7 = latE7;
    m_savedLonE7 = lonE7;
    m_hasSavedPos = true;
    Serial.printf("[GPS] Luu vi tri cuoi %.5f, %.5f\n", m_lat, m_lon);
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
        if (!m_configured) configureModule();
        if (m_cls == 0x01 && m_id == 0x07 && m_len >= 92) onNavPvt(m_buf); // NAV-PVT
        if (m_cls == 0x05 && m_len >= 2 && m_buf[0] == 0x06 && m_buf[1] == 0x8A) {  // ACK/NAK of CFG-VALSET
            if (m_id == 0x01) m_cfgAcked++; else m_cfgNaked++;
            if (m_cfgAcked + m_cfgNaked == m_cfgSent) {
                Serial.printf("[GPS] Cau hinh: %u chap nhan, %u tu choi\n", m_cfgAcked, m_cfgNaked);
            }
        }
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
        m_moving = false;
        m_moveHits = 0;
        return;
    }
    // Speed: Doppler velocity averaged over the last 3 samples (~0.3 s at 10 Hz).
    if (rawVelLog) {
        Serial.printf("[VEL] %lu,%d,%d,%ld,%ld,%ld,%lu,%lu\n", (unsigned long)millis(), fixType, m_numSv,
                      (long)rdI32(p, 48), (long)rdI32(p, 52), (long)rdI32(p, 60), (unsigned long)(uint32_t)rdI32(p, 68),
                      (unsigned long)(uint32_t)rdI32(p, 40));
    }
    (void)sAccMs;   // the P18 Pro reports a fixed 0.06 m/s here, so it cannot be trusted
    uint32_t now = millis();
    float vn = rdI32(p, 48) / 1000.0f, ve = rdI32(p, 52) / 1000.0f;
    float sp = sqrtf(vn * vn + ve * ve);

    // Glitch rejection. Indoors / under multipath the module "teleports": its velocity
    // jumps 0 -> 155 km/h in 0.4 s (~7 g), holds for seconds, then drops back. A car cannot
    // do that, so a jump of more than kJumpMs within ~0.2 s starts a glitch; its samples are
    // ignored until the signal settles near the last trusted velocity again.
    if (m_histCount > 0) {
        float d = sqrtf((vn - m_histN[m_histCount - 1]) * (vn - m_histN[m_histCount - 1]) +
                        (ve - m_histE[m_histCount - 1]) * (ve - m_histE[m_histCount - 1]));
        m_stable = d <= kStableDeltaMs ? (m_stable < 255 ? m_stable + 1 : 255) : 0;
        float j = sqrtf((vn - m_histN[0]) * (vn - m_histN[0]) + (ve - m_histE[0]) * (ve - m_histE[0]));
        if (!m_glitch && m_hasGood && now - m_goodMs < 1500 && now - m_histMs[0] <= 350 && j > kJumpMs) {
            m_glitch = true;
            m_glitchMs = now;
            m_glitchCount++;
        }
    }
    bool accept = true;
    if (m_glitch) {
        float dt = (now - m_goodMs) / 1000.0f;
        float g = sqrtf(m_goodN * m_goodN + m_goodE * m_goodE);
        bool nearGood = sp - g <= kSlackMs + kMaxAccelMs2 * dt && g - sp <= kSlackMs + kMaxDecelMs2 * dt;
        if ((m_stable >= kStableSamples && nearGood) || now - m_glitchMs > kGlitchMaxMs) {
            m_glitch = false;
        } else {
            accept = false;
        }
    }
    if (m_histCount < 2) m_histCount++;
    else { m_histN[0] = m_histN[1]; m_histE[0] = m_histE[1]; m_histMs[0] = m_histMs[1]; }
    m_histN[m_histCount - 1] = vn;
    m_histE[m_histCount - 1] = ve;
    m_histMs[m_histCount - 1] = now;

    if (accept) {
        m_goodN = vn;
        m_goodE = ve;
        m_goodMs = now;
        m_hasGood = true;
        m_vn[m_vIdx] = vn;
        m_ve[m_vIdx] = ve;
        m_vIdx = (m_vIdx + 1) % 3;
        if (m_vN < 3) m_vN++;
    }
    if (m_vN == 0) return;
    float sn = 0, se = 0;
    for (int i = 0; i < m_vN; i++) { sn += m_vn[i]; se += m_ve[i]; }
    sn /= m_vN;
    se /= m_vN;
    float kmh = sqrtf(sn * sn + se * se) * 3.6f;
    m_velMs = now;

    // Start/stop hysteresis: must stay above kStartMovingKmh for 1 s to count as driving,
    // drops to exactly 0 below kStopMovingKmh. Tuned on a 200 s stationary indoor capture.
    if (m_moving) {
        if (kmh < kStopMovingKmh) { m_moving = false; m_moveHits = 0; }
    } else if (kmh >= kStartMovingKmh) {
        if (++m_moveHits >= kStartHits) m_moving = true;
    } else {
        m_moveHits = 0;
    }
    m_speedKmh = m_moving ? kmh : 0.0f;

    // Course only means something while moving; keep the last one when stopped.
    if (m_moving) {
        float c = atan2f(se, sn) * 180.0f / (float)M_PI;
        m_courseDeg = c < 0 ? c + 360.0f : c;
    }
    if (m_hAccM <= kMaxHAccM) {
        // During a glitch the reported position is jumping too: keep the last good one.
        if (!m_glitch || m_ttffMs == 0) {
            m_lon = rdI32(p, 24) / 1e7;
            m_lat = rdI32(p, 28) / 1e7;
            m_heightCm = rdI32(p, 32) / 10;
        }
        m_pvtMs = millis();
        if (m_ttffMs == 0) {
            m_ttffMs = m_pvtMs;
            Serial.printf("[GPS] Dinh vi lan dau sau %.1f s (sats=%d, hAcc=%.0fm)\n", m_ttffMs / 1000.0f, m_numSv,
                          (double)m_hAccM);
        }
        // Remember where we park, for the next power-on
        if (!m_moving && m_hAccM <= kSavePosMaxHAccM) saveLastPosition();
    }
}

bool GPSManager::ubxFresh(uint32_t now) const { return m_pvtMs != 0 && now - m_pvtMs < kFixTimeoutMs; }
bool GPSManager::velFresh(uint32_t now) const { return m_velMs != 0 && now - m_velMs < kFixTimeoutMs; }

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
        Serial.printf("[GPS] %s @%lu  chars=%lu ubx=%lu bad=%lu nmea=%lu  fix=%d sats=%d hAcc=%.1fm spd=%d glitch=%lu\n",
                      getProtocol(), (unsigned long)m_baud, (unsigned long)m_chars,
                      (unsigned long)m_ubxFrames, (unsigned long)m_ubxBad, (unsigned long)passed, hasFix(), getSatellites(),
                      (double)m_hAccM, getSpeedKmh(), (unsigned long)m_glitchCount);
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
    // Speed is usable as soon as the module has a fix, before the position is accurate enough
    if (velFresh(millis())) return (int)lroundf(m_speedKmh);
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
