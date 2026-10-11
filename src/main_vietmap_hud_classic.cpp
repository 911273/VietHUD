/**
 * @file main_vietmap_hud_classic.cpp
 * @brief VietMap Hardware HUD Clone for ESP32 Classic (NodeMCU-32S on COM14)
 * 
 * Target Hardware: ESP32 Classic (ESP32-WROOM-32 / NodeMCU-32S on COM14)
 * Fully aligned with proven VietMap V221 Protocol:
 *   1. Full V2.2.1 frame hashing with MD5 (buildV221Frame).
 *   2. Responds to 0x37 0xC3 with sendV221Ack + sendV221HudStatus.
 *   3. Responds to 0x37 0xC5 with sendV221VersionInfo + sendV221HudStatus.
 *   4. Multi-Characteristic notify (FFF1 + FFF2) with BLE2902 on all chars.
 *   5. BLESecurity Just Works auto-bonding for iOS CoreBluetooth.
 *   6. Single unfragmented frame transmission.
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_bt_main.h"
#include "mbedtls/md5.h"

#ifndef LED_BUILTIN
  #define LED_BUILTIN 2
#endif

#define DEVICE_NAME "VIETMAP_HUD"

#define SERVICE_UUID        "0000fff0-0000-1000-8000-00805f9b34fb"
#define CHAR_TX_UUID        "0000fff1-0000-1000-8000-00805f9b34fb" // Notify / Read to App
#define CHAR_RX_UUID        "0000fff2-0000-1000-8000-00805f9b34fb" // Write from App
#define CHAR_CFG_UUID       "0000fff3-0000-1000-8000-00805f9b34fb" // Config Read/Write
#define CHAR_AUX_UUID       "0000fff4-0000-1000-8000-00805f9b34fb" // Aux Notify

// Frame Header Constants
#define FRAME_HEADER_1      0x55
#define FRAME_HEADER_2      0xAA

static const uint8_t KEY_C[16] = {
    0xF7, 0xF8, 0x15, 0x75, 0x02, 0xDA, 0x52, 0x8E, 
    0xF0, 0xFC, 0x31, 0x39, 0x72, 0x2C, 0xE9, 0xE2
};

BLEServer* pBleServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
BLECharacteristic* pRxCharacteristic = nullptr;
BLECharacteristic* pCfgCharacteristic = nullptr;
BLECharacteristic* pAuxCharacteristic = nullptr;

bool bleConnected = false;
bool handshakeCompleted = false;
bool pendingBleHandshake = false;
uint32_t bleConnectTime = 0;

uint32_t lastHeartbeatMs = 0;
uint32_t lastAdvLogMs = 0;
uint32_t totalPacketsRx = 0;
uint32_t totalPacketsTx = 0;

uint8_t currentSpeed = 0;
uint8_t currentSpeedLimit = 50;
uint16_t currentRpm = 850;
uint8_t currentCoolantTemp = 86;
float batteryVoltage = 13.8f;

static uint8_t bleRxBuf[1024];
static size_t bleRxLen = 0;

// =============================================================================
// BLE SECURITY CALLBACKS (iOS Auto Just Works Pairing)
// =============================================================================
class VietMapSecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override {
        Serial.println("[BLE-SEC] PassKey request -> 0");
        return 0;
    }
    void onPassKeyNotify(uint32_t pass_key) override {
        Serial.printf("[BLE-SEC] PassKey: %d\n", pass_key);
    }
    bool onConfirmPIN(uint32_t pass_key) override {
        Serial.println("[BLE-SEC] Confirm PIN -> Auto Accept");
        return true;
    }
    bool onSecurityRequest() override {
        Serial.println("[BLE-SEC] Security Request from iPhone -> Auto Accept");
        return true;
    }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        if (auth_cmpl.success) {
            Serial.println("[BLE-SEC] === PAIRING / BONDING WITH IPHONE THANH CONG! ===");
        } else {
            Serial.printf("[BLE-SEC] Auth failed: 0x%02X\n", auth_cmpl.fail_reason);
        }
    }
};

// =============================================================================
// SAFE NOTIFY (Dual-Char Tx FFF1 + Rx FFF2)
// =============================================================================
void sendSafeNotify(BLECharacteristic* pChar, const uint8_t* data, size_t len) {
    if (!pChar || !bleConnected) return;
    pChar->setValue((uint8_t*)data, len);
    pChar->notify();
    totalPacketsTx++;
    delay(15);
}

// =============================================================================
// V221 FRAME BUILDER (MD5 HashSum)
// =============================================================================
uint32_t calculateV221HashSum(const uint8_t* rawData, size_t rawLen, const uint8_t* key16) {
    size_t totalHashLen = rawLen + 16;
    uint8_t* hashBuffer = (uint8_t*)malloc(totalHashLen);
    if (!hashBuffer) return 0;

    memcpy(hashBuffer, rawData, rawLen);
    memcpy(hashBuffer + rawLen, key16, 16);

    uint8_t digest[16];
    mbedtls_md5(hashBuffer, totalHashLen, digest);
    free(hashBuffer);

    char md5Hex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(&md5Hex[i * 2], "%02X", digest[i]);
    }
    md5Hex[32] = '\0';

    uint32_t totalSum = 0;
    for (int i = 0; i < 4; i++) {
        char chunk[9];
        memcpy(chunk, &md5Hex[i * 8], 8);
        chunk[8] = '\0';
        uint32_t val = (uint32_t)strtoul(chunk, NULL, 16);
        totalSum += val;
    }
    return totalSum;
}

size_t buildV221Frame(uint8_t cmd, uint8_t sub, const uint8_t* payload, size_t payloadLen, uint32_t ts, uint8_t* outBuf) {
    size_t totalLen = payloadLen + 20;
    uint32_t dataLen = payloadLen + 12;

    outBuf[0] = 0xA5;
    outBuf[1] = 0x5A;
    outBuf[2] = cmd;
    outBuf[3] = sub;

    outBuf[4] = (dataLen >> 24) & 0xFF;
    outBuf[5] = (dataLen >> 16) & 0xFF;
    outBuf[6] = (dataLen >> 8) & 0xFF;
    outBuf[7] = dataLen & 0xFF;

    outBuf[8]  = (ts >> 24) & 0xFF;
    outBuf[9]  = (ts >> 16) & 0xFF;
    outBuf[10] = (ts >> 8) & 0xFF;
    outBuf[11] = ts & 0xFF;

    if (payloadLen > 0 && payload != nullptr) {
        memcpy(&outBuf[12], payload, payloadLen);
    }

    uint32_t sumC = calculateV221HashSum(outBuf, 12 + payloadLen, KEY_C);
    uint32_t sumB = sumC;

    size_t offset = 12 + payloadLen;
    outBuf[offset + 0] = (sumB >> 24) & 0xFF;
    outBuf[offset + 1] = (sumB >> 16) & 0xFF;
    outBuf[offset + 2] = (sumB >> 8)  & 0xFF;
    outBuf[offset + 3] = sumB & 0xFF;

    outBuf[offset + 4] = (sumC >> 24) & 0xFF;
    outBuf[offset + 5] = (sumC >> 16) & 0xFF;
    outBuf[offset + 6] = (sumC >> 8)  & 0xFF;
    outBuf[offset + 7] = sumC & 0xFF;

    return totalLen;
}

// =============================================================================
// PROTOCOL RESPONDERS (Dual-Notify on FFF1 and FFF2)
// =============================================================================

// 1. Generic V2.2.1 ACK (for Time Sync 0x37 0xC3 and other commands)
void sendV221Ack(uint8_t cmd, uint8_t sub, uint32_t ts) {
    uint8_t payload[2] = { 0x00, 0x00 };
    uint8_t outBuf[32];
    size_t len = buildV221Frame(cmd, sub, payload, sizeof(payload), ts, outBuf);
    sendSafeNotify(pTxCharacteristic, outBuf, len);
    sendSafeNotify(pRxCharacteristic, outBuf, len);
    Serial.printf("[TX V221] ACK (Cmd: 0x%02X, Sub: 0x%02X, TS: %u, Len: %d)\n", cmd, sub, ts, len);
}

// 2. Version Info V2.2.1 (for Version Query 0x37 0xC5)
void sendV221VersionInfo(uint8_t reqSub, uint32_t ts) {
    const char* verStr = "MODEL:H1AS,HW:1.0,FW:1.0,PROTOCOL:2.2.1,OBDV:1.0_0,SID:12345678";
    size_t strLen = strlen(verStr);

    uint8_t payload[1 + strLen];
    payload[0] = 0x0E; // Subcommand 14 (Version Info)
    memcpy(&payload[1], verStr, strLen);

    uint8_t outBuf[128];
    size_t len = buildV221Frame(0x37, reqSub, payload, sizeof(payload), ts, outBuf);
    sendSafeNotify(pTxCharacteristic, outBuf, len);
    sendSafeNotify(pRxCharacteristic, outBuf, len);
    Serial.printf("[TX V221] Version Info -> %s (Len: %d)\n", verStr, len);
}

// 3. HUD Status V2.2.1 (0x37 0x53 Keepalive)
void sendV221HudStatus(uint8_t statusByte, uint32_t ts) {
    uint8_t payload[4] = { 0x53, 0x15, statusByte, 0x00 };
    uint8_t outBuf[64];
    size_t len = buildV221Frame(0x37, 0x53, payload, sizeof(payload), ts, outBuf);
    sendSafeNotify(pTxCharacteristic, outBuf, len);
}

// 4. OBD Telemetry V2.2.1
void sendV221ObdTelemetry(uint8_t spd, uint16_t rpm, uint8_t coolant, float volt, uint32_t ts = 0) {
    if (ts == 0) ts = (uint32_t)(millis() / 1000);
    uint8_t payload[6];
    payload[0] = 0x16;
    payload[1] = spd;
    payload[2] = (rpm >> 8) & 0xFF;
    payload[3] = rpm & 0xFF;
    payload[4] = coolant;
    payload[5] = (uint8_t)(volt * 10);

    uint8_t outBuf[64];
    size_t len = buildV221Frame(0x37, 0x16, payload, sizeof(payload), ts, outBuf);
    sendSafeNotify(pTxCharacteristic, outBuf, len);
}

// 5. OEM Legacy HUD_INFO (0x55 0xAA 0x02)
void sendLegacyHudInfoPacket() {
    uint8_t pkt[15];
    pkt[0] = 0x55;
    pkt[1] = 0xAA;
    pkt[2] = 0x02; // CMD_HUD_INFO
    pkt[3] = 0x0A; // Length = 10
    pkt[4] = 'H';
    pkt[5] = '1';
    pkt[6] = 'A';
    pkt[7] = 'S';
    pkt[8] = 0x01; // FW v1.2.0
    pkt[9] = 0x02;
    pkt[10] = 0x00; // 13.8V
    pkt[11] = 138;
    pkt[12] = 0x07; // Status: OBD Ready + GPS Ready + Pro Licensed
    pkt[13] = 0x00;

    uint8_t sum = 0;
    for (int i = 0; i < 14; i++) sum += pkt[i];
    pkt[14] = sum;

    sendSafeNotify(pTxCharacteristic, pkt, 15);
    Serial.println("[TX LEGACY] HUD_INFO (H1AS Pro Active)");
}

void sendLegacyHeartbeatPacket() {
    uint8_t pkt[] = { 0x55, 0xAA, 0x01, 0x01, 0x01, 0x00, 0x03 };
    sendSafeNotify(pTxCharacteristic, pkt, sizeof(pkt));
}

// =============================================================================
// PACKET PROCESSING
// =============================================================================
void processIncomingPacket(const uint8_t* data, size_t len) {
    totalPacketsRx++;

    Serial.printf("[RX #%u] %d bytes: ", totalPacketsRx, len);
    for (size_t i = 0; i < len; i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();

    if (len >= 8 && data[0] == 0xA5 && data[1] == 0x5A) {
        uint8_t cmd = data[2];
        uint8_t sub = data[3];
        uint32_t ts = (len >= 12) ? (((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11]) : (uint32_t)(millis() / 1000);

        // Sub 0xC3: Time Sync -> Reply standard V221 ACK + HUD Status
        if (cmd == 0x37 && sub == 0xC3) {
            Serial.printf("  >>> [APP TIME SYNC 0xC3] TS=%u -> Tra loi V221 ACK + Status...\n", ts);
            sendV221Ack(0x37, 0xC3, ts);
            delay(15);
            sendV221HudStatus(0x01, ts);
            delay(15);
            sendV221VersionInfo(0xC5, ts);
            handshakeCompleted = true;
            digitalWrite(LED_BUILTIN, HIGH);
            return;
        }

        // Sub 0xC5: Version Query -> Reply Version Info + Status
        if (cmd == 0x37 && sub == 0xC5) {
            Serial.println("  >>> [APP HOI VERSION 0xC5] Tra loi Version Info + Status...");
            sendV221VersionInfo(0xC5, ts);
            delay(15);
            sendV221HudStatus(0x01, ts);
            handshakeCompleted = true;
            digitalWrite(LED_BUILTIN, HIGH);
            return;
        }

        // Canh bao toc do / camera / dan duong tu app
        if (cmd == 0x03) {
            uint8_t subType = sub;
            if (subType == 0x04 && len >= 14) {
                currentSpeedLimit = data[12];
                currentSpeed = data[13];
                Serial.printf("  *** [CANH BAO TOC DO] Gioi han: %u km/h | Hien tai: %u km/h ***\n", currentSpeedLimit, currentSpeed);
            }
            else if (subType == 0x05 && len >= 16) {
                uint8_t camType = data[13];
                uint16_t camDist = ((uint16_t)data[14] << 8) | data[15];
                Serial.printf("  *** [CANH BAO CAMERA] Type: %u | Con: %u m ***\n", camType, camDist);
            }
            else if (subType == 0x06 && len >= 16) {
                uint8_t navAction = data[13];
                uint16_t navDist = ((uint16_t)data[14] << 8) | data[15];
                Serial.printf("  *** [DAN DUONG] Huong: 0x%02X | Con: %u m ***\n", navAction, navDist);
            }
        }

        sendV221Ack(cmd, sub, ts);
        return;
    }

    if (data[0] == 0x55 && data[1] == 0xAA) {
        Serial.printf("[RX LEGACY] Cmd: 0x%02X\n", data[2]);
        sendLegacyHeartbeatPacket();
        sendLegacyHudInfoPacket();
    }
}

void parseBleRxStream() {
    while (bleRxLen >= 8) {
        if (bleRxBuf[0] == 0xA5 && bleRxBuf[1] == 0x5A) {
            uint32_t dataLen = ((uint32_t)bleRxBuf[4] << 24) | ((uint32_t)bleRxBuf[5] << 16) | ((uint32_t)bleRxBuf[6] << 8) | bleRxBuf[7];
            size_t frameLen = 8 + dataLen;
            if (frameLen > 256) {
                memmove(bleRxBuf, bleRxBuf + 1, --bleRxLen);
                continue;
            }
            if (bleRxLen < frameLen) break;
            processIncomingPacket(bleRxBuf, frameLen);
            memmove(bleRxBuf, bleRxBuf + frameLen, bleRxLen - frameLen);
            bleRxLen -= frameLen;
        }
        else if (bleRxBuf[0] == 0x55 && bleRxBuf[1] == 0xAA) {
            uint8_t len = bleRxBuf[3];
            size_t frameLen = len + 5;
            if (bleRxLen < frameLen) break;
            processIncomingPacket(bleRxBuf, frameLen);
            memmove(bleRxBuf, bleRxBuf + frameLen, bleRxLen - frameLen);
            bleRxLen -= frameLen;
        }
        else {
            memmove(bleRxBuf, bleRxBuf + 1, --bleRxLen);
        }
    }
}

class MyBleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        bleConnected = true;
        pendingBleHandshake = true;
        bleConnectTime = millis();
        Serial.println("\n********************************************************");
        Serial.println(">>> [BLE] IPHONE DA KET NOI BLE GATT THANH CONG! <<<");
        Serial.println("********************************************************");
    }

    void onDisconnect(BLEServer* pServer) override {
        bleConnected = false;
        handshakeCompleted = false;
        pendingBleHandshake = false;
        bleRxLen = 0;
        digitalWrite(LED_BUILTIN, LOW);
        Serial.println("\n[BLE] !!! iPhone da ngat ket noi BLE !!!");
        BLEDevice::startAdvertising();
        Serial.println("[BLE] Dang quang ba lai (Advertising)...");
    }
};

class MyBleRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
        std::string rxVal = pCharacteristic->getValue();
        size_t len = rxVal.length();
        if (len > 0) {
            const uint8_t* p = (const uint8_t*)rxVal.data();
            for (size_t i = 0; i < len; i++) {
                if (bleRxLen < sizeof(bleRxBuf)) {
                    bleRxBuf[bleRxLen++] = p[i];
                }
            }
            parseBleRxStream();
        }
    }
};

void startFastAdvertising() {
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->stop();

    BLEAdvertisementData advData;
    advData.setFlags(0x06); // General Discoverable + BR/EDR Not Supported
    advData.setCompleteServices(BLEUUID(SERVICE_UUID));
    advData.setName(DEVICE_NAME);
    pAdvertising->setAdvertisementData(advData);

    BLEAdvertisementData scanData;
    scanData.setName(DEVICE_NAME);
    scanData.setCompleteServices(BLEUUID(SERVICE_UUID));
    pAdvertising->setScanResponseData(scanData);

    pAdvertising->setMinPreferred(0x06); // 7.5ms
    pAdvertising->setMaxPreferred(0x12); // 22.5ms

    pAdvertising->start();
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    delay(1000);

    Serial.println("\n");
    Serial.println("=================================================================");
    Serial.println("*    VIETMAP HUD V221 CLONE FOR ESP32 CLASSIC (COM14)           *");
    Serial.println("*    Version 3.0: Full V221 MD5 Frames + iOS BLE Security       *");
    Serial.println("=================================================================");

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_P9);
    BLEDevice::setMTU(517);

    // iOS Security / Pairing
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
    BLEDevice::setSecurityCallbacks(new VietMapSecurityCallbacks());

    BLESecurity* pSecurity = new BLESecurity();
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    pSecurity->setCapability(ESP_IO_CAP_NONE);
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new MyBleServerCallbacks());

    BLEService *pService = pBleServer->createService(SERVICE_UUID);

    // FFF1: TX Notify
    pTxCharacteristic = pService->createCharacteristic(
        CHAR_TX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // FFF2: RX Write & Notify
    pRxCharacteristic = pService->createCharacteristic(
        CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pRxCharacteristic->addDescriptor(new BLE2902());
    pRxCharacteristic->setCallbacks(new MyBleRxCallbacks());

    // FFF3: Config Read/Write
    pCfgCharacteristic = pService->createCharacteristic(
        CHAR_CFG_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pCfgCharacteristic->addDescriptor(new BLE2902());
    uint8_t cfgVal[] = { 0x01, 0x00, 0x00, 0x00 };
    pCfgCharacteristic->setValue(cfgVal, sizeof(cfgVal));

    // FFF4: Aux Notify
    pAuxCharacteristic = pService->createCharacteristic(
        CHAR_AUX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pAuxCharacteristic->addDescriptor(new BLE2902());

    pService->start();

    startFastAdvertising();

    Serial.printf("[SETUP] BLE MAC: %s\n", BLEDevice::getAddress().toString().c_str());
    Serial.printf("[SETUP] Advertising: \"%s\" (Service UUID: 0xFFF0)\n", DEVICE_NAME);
    Serial.println("[SETUP] San sang tiep nhan ket noi tu iPhone VietMap Live!\n");
}

void loop() {
    uint32_t now = millis();

    // Auto trigger handshake sau 400ms khi vua ket noi
    if (bleConnected && pendingBleHandshake && (now - bleConnectTime >= 400)) {
        pendingBleHandshake = false;
        Serial.println("[HANDSHAKE] >>> Kich hoat dinh danh V221 HUD sang iPhone... <<<");
        uint32_t ts = (uint32_t)(now / 1000);
        sendV221VersionInfo(0xC5, ts);
        delay(30);
        sendV221HudStatus(0x01, ts);
        delay(30);
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage, ts);
    }

    // Telemetry & Keepalive dinh ky moi 1 giay khi da ket noi
    if (bleConnected && (now - lastHeartbeatMs >= 1000)) {
        lastHeartbeatMs = now;
        uint32_t ts = (uint32_t)(now / 1000);
        sendV221HudStatus(0x01, ts);
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage, ts);
    }

    // Log trang thai quang ba moi 3 giay khi chua ket noi
    if (!bleConnected && (now - lastAdvLogMs >= 3000)) {
        lastAdvLogMs = now;
        Serial.println("[STATUS] Dang quang ba BLE 'VIETMAP_HUD'... Cho iPhone ket noi...");
    }

    // LED status
    if (!handshakeCompleted) {
        static uint32_t lastBlink = 0;
        if (now - lastBlink > 250) {
            lastBlink = now;
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
    } else {
        digitalWrite(LED_BUILTIN, HIGH);
    }

    delay(10);
}
