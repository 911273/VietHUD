/**
 * @file main_vietmap_hud_classic.cpp
 * @brief VietMap Hardware HUD Clone for ESP32 Classic (Dual-Mode: Classic SPP + BLE GATT)
 * 
 * Target Hardware: ESP32 Classic (NodeMCU-32S, ESP32-WROOM-32)
 * Fixes for "Dang ket noi..." issue:
 *   1. Full FFF0 Service with FFF1 (Tx Notify), FFF2 (Rx Write), FFF3 (Cfg), FFF4 (Aux)
 *   2. Atomic BLE Notifications: No 20-byte chunking! Sent as whole frames.
 *   3. Strict OEM V2.2.1 framing: 8-byte MD5 tail (sumB + sumC), NO trailing 0D 0A.
 *   4. OEM Version Info string: "MODEL:H1AS,HW:1.0,FW:1.0,PROTOCOL:2.2.1,OBDV:1.0_0,SID:12345678"
 *   5. Robust Handshake for both 0xC5 (Version Query) and 0xC3 (Time Sync).
 */

#include <Arduino.h>
#include "BluetoothSerial.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
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

static const uint8_t KEY_C[16] = {
    0xF7, 0xF8, 0x15, 0x75, 0x02, 0xDA, 0x52, 0x8E, 
    0xF0, 0xFC, 0x31, 0x39, 0x72, 0x2C, 0xE9, 0xE2
};

BluetoothSerial SerialBT;
BLEServer* pBleServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
BLECharacteristic* pRxCharacteristic = nullptr;
BLECharacteristic* pCfgCharacteristic = nullptr;
BLECharacteristic* pAuxCharacteristic = nullptr;
BLE2902* pCccdDescriptor = nullptr;

bool sppConnected = false;
bool bleConnected = false;
bool handshakeCompleted = false;
bool pendingBleHandshake = false;
uint32_t bleConnectTime = 0;

uint32_t lastHeartbeatMs = 0;
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

static uint8_t sppRxBuf[1024];
static size_t sppRxLen = 0;

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
// RAW PACKET TRANSMITTER (ATOMIC BLE NOTIFY - NO 20-BYTE CHUNKING)
// =============================================================================
void sendRawPacket(const uint8_t* data, size_t len) {
    if (sppConnected) {
        SerialBT.write(data, len);
    }

    if (bleConnected && pTxCharacteristic != nullptr) {
        pTxCharacteristic->setValue((uint8_t*)data, len);
        pTxCharacteristic->notify();
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
    Serial.printf("[TX V221] HUD Status (0x53, status: %u, Len: %u)\n", statusByte, (unsigned)len);
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
    delay(50);
    sendV221VersionInfo(0xC5, ts);
    delay(40);
    sendV221HudStatus(0x01, ts);
    delay(40);
    sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage, ts);
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
            delay(30);
            sendV221HudStatus(0x01, ts);
            handshakeCompleted = true;
            digitalWrite(LED_BUILTIN, HIGH);
            return;
        }

        // Sub 0xC3: Time Sync
        if (cmd == 0x37 && sub == 0xC3) {
            Serial.printf("  >>> [APP TIME SYNC 0xC3] Dong bo thoi gian TS=%u -> Tra loi ACK + Status + Version...\n", ts);
            sendV221Ack(0x37, 0xC3, ts);
            delay(20);
            sendV221HudStatus(0x01, ts);
            delay(20);
            sendV221VersionInfo(0xC5, ts);
            delay(20);
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

        // Luon ACK cac lenh khac
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
                break; // Cho them byte
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

void parseSppRxStream() {
    while (sppRxLen >= 8) {
        if (sppRxBuf[0] == 0xA5 && sppRxBuf[1] == 0x5A) {
            uint32_t dataLen = ((uint32_t)sppRxBuf[4] << 24) | ((uint32_t)sppRxBuf[5] << 16) | ((uint32_t)sppRxBuf[6] << 8) | sppRxBuf[7];
            size_t frameLen = 8 + dataLen;
            if (frameLen > 256) {
                memmove(sppRxBuf, sppRxBuf + 1, --sppRxLen);
                continue;
            }
            if (sppRxLen < frameLen) break;
            processIncomingPacket(sppRxBuf, frameLen);
            memmove(sppRxBuf, sppRxBuf + frameLen, sppRxLen - frameLen);
            sppRxLen -= frameLen;
        }
        else if (sppRxBuf[0] == 0x55 && sppRxBuf[1] == 0xAA) {
            uint8_t len = sppRxBuf[3];
            size_t frameLen = len + 5;
            if (sppRxLen < frameLen) break;
            processIncomingPacket(sppRxBuf, frameLen);
            memmove(sppRxBuf, sppRxBuf + frameLen, sppRxLen - frameLen);
            sppRxLen -= frameLen;
        }
        else {
            memmove(sppRxBuf, sppRxBuf + 1, --sppRxLen);
        }
    }
}

void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    switch (event) {
        case ESP_SPP_SRV_OPEN_EVT:
            sppConnected = true;
            Serial.println("\n>>> [SPP] Dien thoai da mo cong RFCOMM SPP! <<<");
            triggerHudHandshake();
            break;

        case ESP_SPP_CLOSE_EVT:
            sppConnected = false;
            if (!bleConnected) digitalWrite(LED_BUILTIN, LOW);
            Serial.println("\n[SPP] Dien thoai da ngat ket noi Bluetooth SPP!");
            break;

        default:
            break;
    }
}

class MyBleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        bleConnected = true;
        pendingBleHandshake = true;
        bleConnectTime = millis();
        Serial.println("\n>>> [BLE] Dien thoai da ket noi BLE GATT thanh cong! <<<");
    }

    void onDisconnect(BLEServer* pServer) {
        bleConnected = false;
        handshakeCompleted = false;
        pendingBleHandshake = false;
        bleRxLen = 0;
        if (!sppConnected) digitalWrite(LED_BUILTIN, LOW);
        Serial.println("\n[BLE] Thiet bi da ngat ket noi BLE!");
        BLEDevice::startAdvertising();
        Serial.println("[BLE] Dang quang ba lai BLE...");
    }
};

class MyBleRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
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

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    delay(1000);

    Serial.println("\n");
    Serial.println("*****************************************************************");
    Serial.println("*    VIETMAP HUD CLONE FOR ESP32 CLASSIC (DUAL-MODE BT + BLE)   *");
    Serial.println("*    Version 2.5: Full FFF0 Services + Atomic Frames + H1AS      *");
    Serial.println("*****************************************************************");

    // 1. Khoi tao Bluetooth Classic SPP
    SerialBT.register_callback(sppCallback);
    if (!SerialBT.begin(DEVICE_NAME)) {
        Serial.println("[LOI] Khoi tao Bluetooth Classic SPP that bai!");
    } else {
        Serial.printf("[SETUP] Bluetooth Classic SPP: \"%s\"\n", DEVICE_NAME);
    }
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // 2. Khoi tao Bluetooth Low Energy (BLE)
    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_P9);
    BLEDevice::setMTU(517);

    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new MyBleServerCallbacks());

    BLEService *pService = pBleServer->createService(SERVICE_UUID);

    // FFF1: TX Notify
    pTxCharacteristic = pService->createCharacteristic(
        CHAR_TX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pCccdDescriptor = new BLE2902();
    pCccdDescriptor->setNotifications(true);
    pTxCharacteristic->addDescriptor(pCccdDescriptor);

    // FFF2: RX Write
    pRxCharacteristic = pService->createCharacteristic(
        CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyBleRxCallbacks());

    // FFF3: Config Read/Write (VietMap service discovery requires this)
    pCfgCharacteristic = pService->createCharacteristic(
        CHAR_CFG_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    uint8_t cfgVal[] = { 0x01, 0x00, 0x00, 0x00 };
    pCfgCharacteristic->setValue(cfgVal, sizeof(cfgVal));

    // FFF4: Aux Notify (VietMap service discovery requires this)
    pAuxCharacteristic = pService->createCharacteristic(
        CHAR_AUX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    BLE2902* pAuxCccd = new BLE2902();
    pAuxCccd->setNotifications(true);
    pAuxCharacteristic->addDescriptor(pAuxCccd);

    pService->start();

    // BLE Advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMaxPreferred(0x12);

    BLEAdvertisementData advData;
    advData.setName(DEVICE_NAME);
    advData.setCompleteServices(BLEUUID(SERVICE_UUID));
    pAdvertising->setAdvertisementData(advData);

    BLEAdvertisementData scanData;
    scanData.setName(DEVICE_NAME);
    pAdvertising->setScanResponseData(scanData);

    BLEDevice::startAdvertising();

    Serial.printf("[SETUP] BLE GATT (0xFFF0 FFF1-FFF4) Advertising: \"%s\"\n", DEVICE_NAME);
    Serial.println("[SETUP] He thong san sang ket noi VietMap Live!");
}

void loop() {
    uint32_t now = millis();

    // Doc SPP Classic
    while (SerialBT.available()) {
        uint8_t b = SerialBT.read();
        if (sppRxLen < sizeof(sppRxBuf)) {
            sppRxBuf[sppRxLen++] = b;
        }
    }
    if (sppRxLen > 0) {
        parseSppRxStream();
    }

    // Auto trigger handshake khi BLE vua ket noi sau 250ms (cho phone subscribe CCCD)
    if (bleConnected && pendingBleHandshake && (now - bleConnectTime >= 250)) {
        pendingBleHandshake = false;
        triggerHudHandshake();
    }

    // Telemetry dinh ky moi 1 giay sau khi da handshake
    if ((sppConnected || bleConnected) && handshakeCompleted && (now - lastHeartbeatMs >= 1000)) {
        lastHeartbeatMs = now;
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
    }

    // LED status: Nhap nhay khi chua handshake, Sang lien tuc khi da ket noi
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
