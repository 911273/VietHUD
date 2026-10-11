/**
 * @file main_vietmap_hud_classic.cpp
 * @brief VietMap Hardware HUD Clone for ESP32 Classic (Dual-Mode: Classic SPP + BLE GATT)
 * 
 * Target Hardware: ESP32 Classic (NodeMCU-32S, ESP32-WROOM-32, DOIT DevKit V1)
 * Device Name: VIETMAP_HUD
 * Dual-Mode Support:
 *   1. Bluetooth Classic RFCOMM SPP (UUID 0x1101) for Android Settings & Android App
 *   2. Bluetooth Low Energy BLE GATT (Service 0xFFF0) for iOS / iPhone & Android App
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

#define SERVICE_UUID           "0000FFF0-0000-1000-8000-00805F9B34FB"
#define CHAR_TX_NOTIFY_UUID    "0000FFF1-0000-1000-8000-00805F9B34FB"
#define CHAR_RX_WRITE_UUID     "0000FFF2-0000-1000-8000-00805F9B34FB"

static const uint8_t KEY_C[16] = {
    0xF7, 0xF8, 0x15, 0x75, 0x02, 0xDA, 0x52, 0x8E, 
    0xF0, 0xFC, 0x31, 0x39, 0x72, 0x2C, 0xE9, 0xE2
};

BluetoothSerial SerialBT;
BLEServer* pBleServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
BLECharacteristic* pRxCharacteristic = nullptr;

bool sppConnected = false;
bool bleConnected = false;

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

    size_t sumIdx = 12 + payloadLen;
    outBuf[sumIdx]     = (sumC >> 24) & 0xFF;
    outBuf[sumIdx + 1] = (sumC >> 16) & 0xFF;
    outBuf[sumIdx + 2] = (sumC >> 8)  & 0xFF;
    outBuf[sumIdx + 3] = sumC & 0xFF;

    outBuf[sumIdx + 4] = 0x0D;
    outBuf[sumIdx + 5] = 0x0A;
    outBuf[sumIdx + 6] = 0x0D;
    outBuf[sumIdx + 7] = 0x0A;

    return totalLen;
}

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

void sendV221Ack(uint8_t cmd, uint8_t sub, uint32_t ts) {
    uint8_t ackPayload[2] = { 0x00, 0x00 };
    uint8_t outBuf[32];
    size_t len = buildV221Frame(cmd, sub, ackPayload, 2, ts, outBuf);
    sendRawPacket(outBuf, len);
}

void sendV221VersionInfo(uint8_t sub, uint32_t ts) {
    uint8_t payload[8] = { 0x01, 0x02, 0x02, 0x01, 0x00, 0x01, 0x00, 0x00 };
    uint8_t outBuf[40];
    size_t len = buildV221Frame(0x01, sub, payload, sizeof(payload), ts, outBuf);
    sendRawPacket(outBuf, len);
    Serial.printf("[TX] Gui V2.2.1 Version Info (0x01, sub: 0x%02X)\n", sub);
}

void sendV221HudStatus(uint8_t status, uint32_t ts) {
    uint8_t payload[2] = { status, 0x00 };
    uint8_t outBuf[32];
    size_t len = buildV221Frame(0x01, 0x53, payload, sizeof(payload), ts, outBuf);
    sendRawPacket(outBuf, len);
    Serial.printf("[TX] Gui V2.2.1 HUD Status (0x53, status: %u)\n", status);
}

void sendV221ObdTelemetry(uint8_t spd, uint16_t rpm, uint8_t coolant, float volt) {
    uint8_t payload[8];
    payload[0] = spd;
    payload[1] = (rpm >> 8) & 0xFF;
    payload[2] = rpm & 0xFF;
    payload[3] = coolant;
    uint16_t vInt = (uint16_t)(volt * 10.0f);
    payload[4] = (vInt >> 8) & 0xFF;
    payload[5] = vInt & 0xFF;
    payload[6] = 0x00;
    payload[7] = 0x00;

    uint8_t outBuf[40];
    uint32_t ts = (uint32_t)(millis() / 1000);
    size_t len = buildV221Frame(0x02, 0x16, payload, sizeof(payload), ts, outBuf);
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

void processIncomingPacket(const uint8_t* data, size_t len) {
    totalPacketsRx++;

    if (len >= 20 && data[0] == 0xA5 && data[1] == 0x5A) {
        uint8_t cmd = data[2];
        uint8_t sub = data[3];
        uint32_t dataLen = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
        uint32_t ts = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];

        Serial.printf("[RX V2.2.1] Cmd: 0x%02X, Sub: 0x%02X, DataLen: %u, TS: %u\n", cmd, sub, dataLen, ts);

        if (cmd == 0x01) {
            if (sub == 0xC5 || sub == 0x01) {
                sendV221VersionInfo(sub, ts);
                sendV221HudStatus(0x01, ts);
            }
            else if (sub == 0xC3) {
                Serial.println("  [V2.2.1] App dong bo thoi gian (Time Sync 0xC3) -> OK");
                sendV221Ack(cmd, sub, ts);
            }
        }
        else if (cmd == 0x03) {
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
        if (cmd == 0x01) sendLegacyHeartbeatPacket();
        else if (cmd == 0x02) sendLegacyHudInfoPacket();
    }
}

// Bluetooth Classic SPP Callback
void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    switch (event) {
        case ESP_SPP_SRV_OPEN_EVT:
            sppConnected = true;
            digitalWrite(LED_BUILTIN, HIGH);
            Serial.println("\n========================================================");
            Serial.println(">>> DIEN THOAI / MAY TINH BANG DA KET NOI SPP CLASSIC! <<<");
            Serial.println(">>> VIETMAP LIVE APP PRO DUOC KICH HOAT THANH CONG! <<<");
            Serial.println("========================================================\n");
            delay(100);
            sendV221VersionInfo(0xC5, (uint32_t)(millis() / 1000));
            delay(30);
            sendV221HudStatus(0x01, (uint32_t)(millis() / 1000));
            delay(30);
            sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
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

// BLE Callbacks
class MyBleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        bleConnected = true;
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.println("\n========================================================");
        Serial.println(">>> DIEN THOAI / IPHONE DA KET NOI BLE GATT (0xFFF0)! <<<");
        Serial.println(">>> VIETMAP LIVE APP PRO DUOC KICH HOAT THANH CONG! <<<");
        Serial.println("========================================================\n");
        delay(100);
        sendV221VersionInfo(0xC5, (uint32_t)(millis() / 1000));
        delay(30);
        sendV221HudStatus(0x01, (uint32_t)(millis() / 1000));
        delay(30);
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
    }

    void onDisconnect(BLEServer* pServer) {
        bleConnected = false;
        if (!sppConnected) digitalWrite(LED_BUILTIN, LOW);
        Serial.println("\n[BLE] Thiet bi da ngat ket noi BLE!");
        BLEDevice::startAdvertising();
        Serial.println("[BLE] Dang phat lai quang ba BLE...");
    }
};

class MyBleRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string rxVal = pCharacteristic->getValue();
        if (rxVal.length() > 0) {
            processIncomingPacket((const uint8_t*)rxVal.data(), rxVal.length());
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
    Serial.println("*    Model: VIETMAP_HUD (H1N) | Tuong thich ca Android va iPhone*");
    Serial.println("*    1. Bluetooth Classic RFCOMM SPP (0x1101) cho Android        *");
    Serial.println("*    2. Bluetooth Low Energy GATT (0xFFF0) cho iOS & App Scan   *");
    Serial.println("*****************************************************************");

    // 1. Khoi tao Bluetooth Classic SPP
    SerialBT.register_callback(sppCallback);
    if (!SerialBT.begin(DEVICE_NAME)) {
        Serial.println("[LOI] Khoi tao Bluetooth Classic SPP that bai!");
    } else {
        Serial.printf("[SETUP] Bluetooth Classic SPP da san sang voi ten: \"%s\"\n", DEVICE_NAME);
    }

    // Set Classic discoverable & connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // 2. Khoi tao Bluetooth Low Energy (BLE)
    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_P9);

    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new MyBleServerCallbacks());

    BLEService *pService = pBleServer->createService(SERVICE_UUID);

    pTxCharacteristic = pService->createCharacteristic(
        CHAR_TX_NOTIFY_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    pRxCharacteristic = pService->createCharacteristic(
        CHAR_RX_WRITE_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyBleRxCallbacks());

    pService->start();

    // Setup BLE Advertising
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

    Serial.printf("[SETUP] BLE GATT (0xFFF0) da phat song quang ba voi ten: \"%s\"\n", DEVICE_NAME);
    Serial.println("[SETUP] Tat ca kenh da san sang! Hay mo Bluetooth tren dien thoai de tim kiem.");
}

static uint8_t sppRxBuf[512];
static size_t sppRxLen = 0;

void loop() {
    uint32_t now = millis();

    // Read from Classic SPP
    while (SerialBT.available()) {
        uint8_t b = SerialBT.read();
        if (sppRxLen < sizeof(sppRxBuf)) {
            sppRxBuf[sppRxLen++] = b;
        }

        if (sppRxLen >= 20 && sppRxBuf[0] == 0xA5 && sppRxBuf[1] == 0x5A) {
            uint32_t dataLen = ((uint32_t)sppRxBuf[4] << 24) | ((uint32_t)sppRxBuf[5] << 16) | ((uint32_t)sppRxBuf[6] << 8) | sppRxBuf[7];
            size_t expectedTotal = 8 + dataLen;
            if (sppRxLen >= expectedTotal) {
                processIncomingPacket(sppRxBuf, expectedTotal);
                sppRxLen = 0;
            }
        }
        else if (sppRxLen >= 8 && sppRxBuf[0] == 0x55 && sppRxBuf[1] == 0xAA) {
            uint8_t len = sppRxBuf[3];
            size_t expectedTotal = len + 5;
            if (sppRxLen >= expectedTotal) {
                processIncomingPacket(sppRxBuf, expectedTotal);
                sppRxLen = 0;
            }
        }
    }

    // Periodic Keepalive / Telemetry
    if ((sppConnected || bleConnected) && (now - lastHeartbeatMs >= 1000)) {
        lastHeartbeatMs = now;
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
    }

    // LED status indicator
    if (!sppConnected && !bleConnected) {
        static uint32_t lastBlink = 0;
        if (now - lastBlink > 400) {
            lastBlink = now;
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
    }

    delay(10);
}
