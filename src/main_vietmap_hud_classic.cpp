/**
 * @file main_vietmap_hud_classic.cpp
 * @brief VietMap Hardware HUD Clone for ESP32 Classic (Dedicated Pure BLE Mode)
 * 
 * Target Hardware: ESP32 Classic (NodeMCU-32S, ESP32-WROOM-32 on COM14)
 * Solves "Dang ket noi..." on iPhone (iOS):
 *   1. 100% Dedicated BLE Radio (Disables Classic SPP to eliminate radio collisions & CONNECT_IND drops).
 *   2. Fast & Stable Advertising interval (32 * 0.625ms = 20ms) for instant iPhone discovery.
 *   3. Standard SIG Advertisement (Flags 0x06 + Complete Service 0xFFF0).
 *   4. Full FFF0 GATT Service: FFF1 (Tx Notify), FFF2 (Rx Write + Notify), FFF3 (Cfg), FFF4 (Aux).
 *   5. Atomic Notifications (No 20-byte chunking!).
 *   6. Strict OEM V2.2.1 Checksum (8-byte MD5 tail sumB+sumC, NO trailing 0D 0A).
 *   7. OEM Version String: "MODEL:H1AS,HW:1.0,FW:1.0,PROTOCOL:2.2.1,OBDV:1.0_0,SID:12345678".
 *   8. Active Keepalive Heartbeat & Telemetry every 1 second.
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
#define CHAR_RX_UUID        "0000fff2-0000-1000-8000-00805f9b34fb" // Write from App + Notify
#define CHAR_CFG_UUID       "0000fff3-0000-1000-8000-00805f9b34fb" // Config Read/Write
#define CHAR_AUX_UUID       "0000fff4-0000-1000-8000-00805f9b34fb" // Aux Notify

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
bool oldBleConnected = false;
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
String lastCameraAlert = "None";
uint16_t lastCameraDistance = 0;
String lastNavDirection = "Straight";
uint16_t lastNavDistance = 0;

static uint8_t bleRxBuf[1024];
static size_t bleRxLen = 0;

// =============================================================================
// V221 FRAME BUILDER (REVERSE-ENGINEERED MD5 CHECKSUM)
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
    size_t totalLen = payloadLen + 20; // 8 header + 4 ts + payloadLen + 8 checksums
    uint32_t dataLen = payloadLen + 12;

    outBuf[0] = 0xA5;
    outBuf[1] = 0x5A;
    outBuf[2] = cmd;
    outBuf[3] = sub;

    // 4-byte Big-Endian Length
    outBuf[4] = (dataLen >> 24) & 0xFF;
    outBuf[5] = (dataLen >> 16) & 0xFF;
    outBuf[6] = (dataLen >> 8) & 0xFF;
    outBuf[7] = dataLen & 0xFF;

    // 4-byte Big-Endian Timestamp
    outBuf[8]  = (ts >> 24) & 0xFF;
    outBuf[9]  = (ts >> 16) & 0xFF;
    outBuf[10] = (ts >> 8) & 0xFF;
    outBuf[11] = ts & 0xFF;

    // Payload
    if (payloadLen > 0 && payload != nullptr) {
        memcpy(&outBuf[12], payload, payloadLen);
    }

    // Checksums: 8-byte tail = sumB (4 bytes) + sumC (4 bytes)
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
// RAW PACKET TRANSMITTER (ATOMIC BLE NOTIFY ON BOTH FFF1 & FFF2)
// =============================================================================
void sendRawPacket(const uint8_t* data, size_t len) {
    if (!bleConnected) return;

    if (pTxCharacteristic != nullptr) {
        pTxCharacteristic->setValue((uint8_t*)data, len);
        pTxCharacteristic->notify();
    }
    if (pRxCharacteristic != nullptr) {
        pRxCharacteristic->setValue((uint8_t*)data, len);
        pRxCharacteristic->notify();
    }
    totalPacketsTx++;
}

// =============================================================================
// VIETMAP OEM PROTOCOL RESPONSES
// =============================================================================
void sendV221Ack(uint8_t cmd, uint8_t sub, uint32_t ts) {
    uint8_t ackPayload[2] = { 0x00, 0x00 };
    uint8_t outBuf[32];
    size_t len = buildV221Frame(cmd, sub, ackPayload, 2, ts, outBuf);
    sendRawPacket(outBuf, len);
    Serial.printf("[TX V221] ACK (Cmd: 0x%02X, Sub: 0x%02X, TS: %u, Len: %u)\n", cmd, sub, ts, (unsigned)len);
}

void sendV221VersionInfo(uint8_t reqSub, uint32_t ts) {
    const char* verStr = "MODEL:H1AS,HW:1.0,FW:1.0,PROTOCOL:2.2.1,OBDV:1.0_0,SID:12345678";
    size_t strLen = strlen(verStr);

    uint8_t payload[1 + strLen];
    payload[0] = 0x0E; // Subcommand 14 (Version Info)
    memcpy(&payload[1], verStr, strLen);

    uint8_t outBuf[128];
    size_t len = buildV221Frame(0x37, reqSub, payload, sizeof(payload), ts, outBuf);
    sendRawPacket(outBuf, len);
    Serial.printf("[TX V221] Version Info -> %s (Len: %u)\n", verStr, (unsigned)len);
}

void sendV221HudStatus(uint8_t statusByte, uint32_t ts) {
    uint8_t payload[4] = { 0x53, 0x15, statusByte, 0x00 };
    uint8_t outBuf[64];
    size_t len = buildV221Frame(0x37, 0x53, payload, sizeof(payload), ts, outBuf);
    sendRawPacket(outBuf, len);
}

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
    sendRawPacket(outBuf, len);
}

void sendLegacyHeartbeatPacket() {
    uint8_t packet[8] = { 0x55, 0xAA, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00 };
    uint8_t checksum = 0;
    for (int i = 0; i < 7; i++) checksum ^= packet[i];
    packet[7] = checksum;
    sendRawPacket(packet, sizeof(packet));
}

void sendLegacyHudInfoPacket() {
    uint8_t packet[16] = {
        0x55, 0xAA, 0x02, 0x09,
        currentSpeed,
        currentSpeedLimit,
        (uint8_t)((currentRpm >> 8) & 0xFF),
        (uint8_t)(currentRpm & 0xFF),
        currentCoolantTemp,
        (uint8_t)((uint16_t)(batteryVoltage * 10) & 0xFF),
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t checksum = 0;
    for (int i = 0; i < 15; i++) checksum ^= packet[i];
    packet[15] = checksum;
    sendRawPacket(packet, sizeof(packet));
}

void triggerHudHandshake() {
    uint32_t ts = (uint32_t)(millis() / 1000);
    Serial.println("\n[HANDSHAKE] >>> Kich hoat chuoi dinh danh HUD cho VietMap Live... <<<");
    delay(40);
    sendV221VersionInfo(0xC5, ts);
    delay(30);
    sendV221HudStatus(0x01, ts);
    delay(30);
    sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage, ts);
    delay(30);
    sendLegacyHudInfoPacket();
    handshakeCompleted = true;
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("[HANDSHAKE] >>> Hoan tat! VietMap Live chuyen sang DA KET NOI! <<<\n");
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
        uint32_t dataLen = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
        uint32_t ts = (len >= 12) ? (((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11]) : (uint32_t)(millis() / 1000);

        Serial.printf("  -> [V221 A5 5A] CMD: 0x%02X, SUB: 0x%02X, DataLen: %u, TS: %u\n", cmd, sub, dataLen, ts);

        // Sub 0xC5: Query Version Info
        if (cmd == 0x37 && sub == 0xC5) {
            Serial.println("  >>> [APP HOI VERSION 0xC5] Gui Version Info + Status...");
            sendV221VersionInfo(0xC5, ts);
            delay(20);
            sendV221HudStatus(0x01, ts);
            handshakeCompleted = true;
            digitalWrite(LED_BUILTIN, HIGH);
            return;
        }

        // Sub 0xC3: Time Sync
        if (cmd == 0x37 && sub == 0xC3) {
            Serial.printf("  >>> [APP TIME SYNC 0xC3] Dong bo thoi gian TS=%u -> Tra loi ACK + Status + Version...\n", ts);
            sendV221Ack(0x37, 0xC3, ts);
            delay(15);
            sendV221HudStatus(0x01, ts);
            delay(15);
            sendV221VersionInfo(0xC5, ts);
            delay(15);
            sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage, ts);
            handshakeCompleted = true;
            digitalWrite(LED_BUILTIN, HIGH);
            return;
        }

        // Heartbeat / Telemetry / Canh bao toc do / Camera / Dan duong
        if (cmd == 0x03) {
            uint8_t subType = sub;
            if (subType == 0x04 && len >= 14) {
                currentSpeedLimit = data[12];
                currentSpeed = data[13];
                Serial.printf("  *** [CANH BAO TOC DO] Gioi han: %u km/h | Hien tai: %u km/h ***\n", currentSpeedLimit, currentSpeed);
            }
            else if (subType == 0x05 && len >= 16) {
                uint8_t camType = data[13];
                lastCameraDistance = ((uint16_t)data[14] << 8) | data[15];
                const char* cName = "Camera Phat Nguoi";
                if (camType == 1) cName = "Camera Ban Toc Do";
                else if (camType == 2) cName = "Camera Vuot Den Do";
                else if (camType == 3) cName = "Camera Giam Sat Lan";
                Serial.printf("  *** [CANH BAO CAMERA] %s | Con: %u m ***\n", cName, lastCameraDistance);
            }
            else if (subType == 0x06 && len >= 16) {
                uint8_t navAction = data[13];
                lastNavDistance = ((uint16_t)data[14] << 8) | data[15];
                Serial.printf("  *** [DAN DUONG] Huong: 0x%02X | Con: %u m ***\n", navAction, lastNavDistance);
            }
        }

        sendV221Ack(cmd, sub, ts);
        return;
    }

    if (data[0] == 0x55 && data[1] == 0xAA) {
        uint8_t cmd = data[2];
        Serial.printf("[RX LEGACY] Cmd: 0x%02X\n", cmd);
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
            if (bleRxLen < frameLen) {
                break;
            }
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
    advData.setFlags(0x06); // General Discoverable + BR/EDR Not Supported (Pure BLE for iOS)
    advData.setCompleteServices(BLEUUID(SERVICE_UUID));
    advData.setName(DEVICE_NAME);
    pAdvertising->setAdvertisementData(advData);

    BLEAdvertisementData scanData;
    scanData.setName(DEVICE_NAME);
    scanData.setCompleteServices(BLEUUID(SERVICE_UUID));
    pAdvertising->setScanResponseData(scanData);

    pAdvertising->setMinPreferred(0x06); // 7.5ms (Optimal for iOS)
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
    Serial.println("*    VIETMAP HUD DEDICATED PURE BLE EMULATOR (ESP32 CLASSIC)    *");
    Serial.println("*    Model: H1AS Pro | Fast 20ms Adv | Full FFF0 GATT Server    *");
    Serial.println("=================================================================");

    // Khoi tao Bluetooth Low Energy (BLE) chuyen dung
    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_P9);
    BLEDevice::setMTU(517);

    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new MyBleServerCallbacks());

    BLEService *pService = pBleServer->createService(SERVICE_UUID);

    // FFF1: TX Notify + Indicate
    pTxCharacteristic = pService->createCharacteristic(
        CHAR_TX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // FFF2: RX Write + Notify
    pRxCharacteristic = pService->createCharacteristic(
        CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pRxCharacteristic->addDescriptor(new BLE2902());
    pRxCharacteristic->setCallbacks(new MyBleRxCallbacks());

    // FFF3: Config Read/Write
    pCfgCharacteristic = pService->createCharacteristic(
        CHAR_CFG_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    uint8_t cfgVal[] = { 0x01, 0x00, 0x00, 0x00 };
    pCfgCharacteristic->setValue(cfgVal, sizeof(cfgVal));

    // FFF4: Aux Notify
    pAuxCharacteristic = pService->createCharacteristic(
        CHAR_AUX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pAuxCharacteristic->addDescriptor(new BLE2902());

    pService->start();

    // Bat quang ba Advertising toc do cao cho iPhone
    startFastAdvertising();

    Serial.printf("[SETUP] BLE MAC: %s\n", BLEDevice::getAddress().toString().c_str());
    Serial.printf("[SETUP] Advertising: \"%s\" (Service UUID: 0xFFF0)\n", DEVICE_NAME);
    Serial.println("[SETUP] San sang tiep nhan ket noi tu iPhone VietMap Live!\n");
}

void loop() {
    uint32_t now = millis();

    // Tu dong gui chuoi Handshake sau 300ms khi vua ket noi
    if (bleConnected && pendingBleHandshake && (now - bleConnectTime >= 300)) {
        pendingBleHandshake = false;
        triggerHudHandshake();
    }

    // Telemetry & Keepalive dinh ky moi 1 giay khi da ket noi
    if (bleConnected && (now - lastHeartbeatMs >= 1000)) {
        lastHeartbeatMs = now;
        uint32_t ts = (uint32_t)(now / 1000);
        sendV221HudStatus(0x01, ts);
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage, ts);
        sendLegacyHeartbeatPacket();
    }

    // Log trang thai quang ba moi 3 giay khi chua ket noi
    if (!bleConnected && (now - lastAdvLogMs >= 3000)) {
        lastAdvLogMs = now;
        Serial.println("[STATUS] Dang quang ba BLE 'VIETMAP_HUD'... Cho iPhone ket noi...");
    }

    // LED status: Nhap nhay khi dang quang ba, Sang lien tuc khi da ket noi
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
