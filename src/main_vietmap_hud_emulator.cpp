/**
 * @file main_vietmap_hud_emulator.cpp
 * @brief VietMap Hardware HUD Clone & BLE Emulator for ESP32-S3
 * 
 * Clones VietMap HUD (Models: H1N / H2AS, Protocol V221 / V2.2.1) to activate and maintain
 * continuous connection with VietMap Live (iOS & Android) and unlock HUD Pro features.
 * 
 * Hardware: ESP32-S3 (Generic / JC3248W535)
 * BLE Service: 0xFFF0 (Custom VietMap HUD GATT Protocol)
 * TX Char (Notify): 0xFFF1
 * RX Char (Write):  0xFFF2
 * Config Char:      0xFFF3
 * Aux Char (Notify):0xFFF4
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_gap_ble_api.h>
#include "mbedtls/md5.h"

// =============================================================================
// BLUETOOTH BLE DEFINITIONS & GATT UUIDS
// =============================================================================
#define DEVICE_NAME         "VIETMAP_HUD"

#define SERVICE_UUID        "0000fff0-0000-1000-8000-00805f9b34fb"
#define CHAR_TX_UUID        "0000fff1-0000-1000-8000-00805f9b34fb" // Notify / Read to App
#define CHAR_RX_UUID        "0000fff2-0000-1000-8000-00805f9b34fb" // Write from App
#define CHAR_CFG_UUID       "0000fff3-0000-1000-8000-00805f9b34fb" // Config Read/Write
#define CHAR_AUX_UUID       "0000fff4-0000-1000-8000-00805f9b34fb" // Aux Notify

// Frame Header & Protocol Constants
#define FRAME_HEADER_1      0x55
#define FRAME_HEADER_2      0xAA

// Legacy 55 AA Command IDs
#define CMD_HEARTBEAT       0x01
#define CMD_HUD_INFO        0x02
#define CMD_OBD_TELEMETRY   0x03
#define CMD_SPEED_LIMIT     0x04
#define CMD_CAMERA_ALERT    0x05
#define CMD_NAVIGATION      0x06
#define CMD_TPMS_INFO       0x07
#define CMD_ACK             0xFF

// =============================================================================
// VIETMAP V221 OEM PROTOCOL CONSTANTS & SECURITY KEY
// =============================================================================
// 16-byte fixed Key C extracted from decompiled VietMap Live SDK (LV7/c.c)
static const uint8_t KEY_C[16] = {
    0xF7, 0xFD, 0x15, 0x75, 0x02, 0xDA, 0x52, 0x8E, 
    0xF0, 0xFC, 0x31, 0x39, 0x72, 0x2C, 0xE9, 0xE2
};

// Global BLE State
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
BLECharacteristic* pRxCharacteristic = nullptr;
BLECharacteristic* pCfgCharacteristic = nullptr;
BLECharacteristic* pAuxCharacteristic = nullptr;

bool deviceConnected = false;
bool oldDeviceConnected = false;
char currentDevName[32] = DEVICE_NAME;
uint32_t lastHeartbeatMs = 0;
uint32_t totalPacketsRx = 0;
uint32_t totalPacketsTx = 0;

// Telemetry State
uint8_t currentSpeed = 0;
uint8_t currentSpeedLimit = 50;
uint16_t currentRpm = 850;
uint8_t currentCoolantTemp = 86; // 86 deg C
float batteryVoltage = 13.8f;    // 13.8V
String lastCameraAlert = "None";
uint16_t lastCameraDistance = 0;
String lastNavDirection = "Straight";
uint16_t lastNavDistance = 0;

// =============================================================================
// V221 FRAME BUILDER WITH REVERSE-ENGINEERED MD5 CHECKSUM
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
    size_t totalLen = payloadLen + 20; // 12 header + payload + 8 checksums
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

    // Checksums
    uint32_t sumC = calculateV221HashSum(outBuf, 12 + payloadLen, KEY_C);
    uint32_t sumB = sumC; // Default session key matches Key C

    size_t offset = 12 + payloadLen;
    // 4 bytes sumB
    outBuf[offset + 0] = (sumB >> 24) & 0xFF;
    outBuf[offset + 1] = (sumB >> 16) & 0xFF;
    outBuf[offset + 2] = (sumB >> 8) & 0xFF;
    outBuf[offset + 3] = sumB & 0xFF;

    // 4 bytes sumC
    outBuf[offset + 4] = (sumC >> 24) & 0xFF;
    outBuf[offset + 5] = (sumC >> 16) & 0xFF;
    outBuf[offset + 6] = (sumC >> 8) & 0xFF;
    outBuf[offset + 7] = sumC & 0xFF;

    return totalLen;
}

void sendSafeNotify(BLECharacteristic* pChar, const uint8_t* data, size_t len) {
    if (!pChar || !deviceConnected) return;
    pChar->setValue((uint8_t*)data, len);
    pChar->notify();
    totalPacketsTx++;
    delay(20);
}

// Helper: Calculate 55 AA Checksum
uint8_t calculateChecksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

// Forward Declarations
void sendV221VersionInfo(uint8_t reqSub = 0xC5, uint32_t ts = 0);
void sendV221HudStatus(uint8_t statusByte = 0x01, uint32_t ts = 0);
void sendV221ObdTelemetry(uint8_t speed, uint16_t rpm, uint8_t coolant, float voltage, uint32_t ts = 0);
void sendV221Ack(uint8_t cmd, uint8_t sub, uint32_t ts = 0);
void sendLegacyHudInfoPacket();
void sendLegacyHeartbeatPacket();
void sendLegacyObdTelemetryPacket();

void parseIncomingPacket(const uint8_t* data, size_t len);
void startRawAdvertising(const char* name = nullptr);

static volatile bool advertising = false;

// =============================================================================
// BLE SERVER CALLBACKS
// =============================================================================
class VietMapServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        deviceConnected = true;
        advertising = false;
        Serial.println("\n[BLE] ==================================================");
        Serial.println("[BLE] >>> VIETMAP LIVE APP DA KET NOI THANH CONG! <<<");
        Serial.println("[BLE] Kich hoat bat tay & duy tri ket noi VietMap Pro...");
        Serial.println("[BLE] ==================================================\n");
        
        delay(80);
        // Gui goi tin Version Info chuan V221
        sendV221VersionInfo(0xC5, (uint32_t)(millis() / 1000));
        delay(40);
        // Gui trang thai HUD binh thuong
        sendV221HudStatus(0x01, (uint32_t)(millis() / 1000));
        delay(40);
        // Gui thong so OBD de kich hoat telemetry
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
        delay(40);
        // Dong thoi gui legacy info
        sendLegacyHudInfoPacket();
    }

    void onDisconnect(BLEServer* pServer) override {
        deviceConnected = false;
        Serial.println("\n[BLE] !!! VIETMAP LIVE APP DA NGAT KET NOI !!!");
        Serial.println("[BLE] Tu dong quang ba lai cho ket noi tiep theo...");
    }
};

// =============================================================================
// BLE RX CHARACTERISTIC CALLBACKS
// =============================================================================
class VietMapRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string rxValue = pCharacteristic->getValue();
        size_t len = rxValue.length();
        if (len > 0) {
            totalPacketsRx++;
            const uint8_t* data = (const uint8_t*)rxValue.data();
            
            Serial.printf("[RX #%u] %d bytes: ", totalPacketsRx, len);
            for (size_t i = 0; i < len; i++) {
                Serial.printf("%02X ", data[i]);
            }
            Serial.println();

            parseIncomingPacket(data, len);
        }
    }
};

// =============================================================================
// PROTOCOL PACKET PARSER & DECODER
// =============================================================================
void parseIncomingPacket(const uint8_t* data, size_t len) {
    if (len < 2) return;

    // 1. VIETMAP LIVE A5 5A PROTOCOL (V221 / V222 / V22S)
    if (data[0] == 0xA5 && data[1] == 0x5A) {
        uint8_t cmd = (len >= 3) ? data[2] : 0x00;
        uint8_t sub = (len >= 4) ? data[3] : 0x00;
        uint32_t ts = 0;
        if (len >= 12) {
            ts = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
        }

        Serial.printf("  -> [V221 A5 5A] CMD: 0x%02X, SUB: 0x%02X, TS: %u (Total %d bytes)\n", cmd, sub, ts, len);

        if (!pTxCharacteristic) return;

        // XU LY TRUY VAN THONG TIN MAY (QUERY HUD MACHINE INFO): CMD 0x37, SUB 0xC5
        if (cmd == 0x37 && sub == 0xC5) {
            Serial.println("  [HANDSHAKE 0xC5] App yeu cau Version Info -> Gui MODEL:H1N PROTOCOL:2.2.1");
            sendV221VersionInfo(0xC5, ts);
            delay(25);
            sendV221HudStatus(0x01, ts);
            return;
        }

        // XU LY DONG BO THOI GIAN (TIME SYNC / 1Hz HEARTBEAT): CMD 0x37, SUB 0xC3
        if (cmd == 0x37 && sub == 0xC3) {
            Serial.printf("  [TIME SYNC 0xC3] Nhan timestamp tu app: %u -> Phan hoi ACK\n", ts);
            sendV221Ack(0x37, 0xC3, ts);
            delay(15);
            sendV221HudStatus(0x01, ts);
            return;
        }

        // XU LY HEARTBEAT KEEPALIVE: CMD 0x02
        if (len >= 13 && data[12] == 0x02) {
            Serial.println("  [HEARTBEAT 0x02] VietMap Ping Keepalive -> Phan hoi HUD Status 0x53");
            sendV221HudStatus(0x01, ts);
            return;
        }

        // XU LY CANH BAO CAMERA / TOC DO / DAN DUONG
        if (len >= 13) {
            uint8_t subType = data[12];
            // 0x04 = Speed Limit, 0x05 = Camera, 0x06 = Navigation
            if (subType == 0x04 && len >= 14) {
                currentSpeedLimit = data[13];
                Serial.printf("  *** [CANH BAO] TOC DO GIOI HAN: %u km/h ***\n", currentSpeedLimit);
            }
            else if (subType == 0x05 && len >= 16) {
                uint8_t camType = data[13];
                lastCameraDistance = ((uint16_t)data[14] << 8) | data[15];
                const char* cName = "Camera Phat Nguoi";
                if (camType == 1) cName = "Camera Ban Toc Do";
                else if (camType == 2) cName = "Camera Vuot Den Do";
                else if (camType == 3) cName = "Camera Giam Sat Lan";
                lastCameraAlert = String(cName);
                Serial.printf("  *** [CANH BAO CAMERA] %s | Con: %u m ***\n", cName, lastCameraDistance);
            }
            else if (subType == 0x06 && len >= 16) {
                uint8_t navAction = data[13];
                lastNavDistance = ((uint16_t)data[14] << 8) | data[15];
                Serial.printf("  *** [DAN DUONG] Huong: 0x%02X | Con: %u m ***\n", navAction, lastNavDistance);
            }
        }

        // Phan hoi Generic ACK de app khong bao gio bi timeout treo ket noi
        sendV221Ack(cmd, sub, ts);
        return;
    }

    // 2. LEGACY 55 AA PROTOCOL (Fallback)
    if (data[0] == FRAME_HEADER_1 && data[1] == FRAME_HEADER_2) {
        uint8_t cmd = data[2];
        switch (cmd) {
            case CMD_HEARTBEAT:
                sendLegacyHeartbeatPacket();
                break;
            case CMD_HUD_INFO:
                sendLegacyHudInfoPacket();
                break;
            case CMD_SPEED_LIMIT:
                if (len >= 4) currentSpeedLimit = data[3];
                break;
            case CMD_CAMERA_ALERT:
                if (len >= 6) {
                    lastCameraDistance = (data[4] << 8) | data[5];
                }
                break;
            case CMD_NAVIGATION:
                if (len >= 6) {
                    lastNavDistance = (data[4] << 8) | data[5];
                }
                break;
            default:
                uint8_t ackPkt[] = {FRAME_HEADER_1, FRAME_HEADER_2, CMD_ACK, 0x01, cmd, 0x00};
                ackPkt[5] = calculateChecksum(ackPkt, 5);
                sendSafeNotify(pTxCharacteristic, ackPkt, sizeof(ackPkt));
                break;
        }
    }
}

// =============================================================================
// TRANSMITTER IMPLEMENTATIONS
// =============================================================================
void sendV221VersionInfo(uint8_t reqSub, uint32_t ts) {
    if (!deviceConnected || !pTxCharacteristic) return;

    // Chuoi Version Info chuan theo giai ma Dalvik LP7/a.x
    const char* verStr = "MODEL:H1N,HW:1.0,FW:1.0,PROTOCOL:2.2.1,OBDV:1.0_0,SID:12345678";
    size_t strLen = strlen(verStr);

    uint8_t payload[1 + strLen];
    payload[0] = 0x0E; // Subcommand 14 (Version Info)
    memcpy(&payload[1], verStr, strLen);

    uint8_t outBuf[128];
    size_t frameLen = buildV221Frame(0x37, reqSub, payload, sizeof(payload), ts, outBuf);

    sendSafeNotify(pTxCharacteristic, outBuf, frameLen);
    sendSafeNotify(pRxCharacteristic, outBuf, frameLen); // Gui them tren RX cho chac chan

    Serial.printf("[TX V221] >>> Da gui Version Info (Len: %u, PROTOCOL: 2.2.1) <<<\n", frameLen);
}

void sendV221HudStatus(uint8_t statusByte, uint32_t ts) {
    if (!deviceConnected || !pTxCharacteristic) return;

    // Subcommand 0x53 (83): HUD Status
    uint8_t payload[4];
    payload[0] = 0x53; // 83
    payload[1] = 0x15; // 21
    payload[2] = statusByte; // 0x01 = Active/Ready
    payload[3] = 0x00;

    uint8_t outBuf[64];
    size_t frameLen = buildV221Frame(0x37, 0x53, payload, sizeof(payload), ts, outBuf);

    sendSafeNotify(pTxCharacteristic, outBuf, frameLen);
}

void sendV221ObdTelemetry(uint8_t speed, uint16_t rpm, uint8_t coolant, float voltage, uint32_t ts) {
    if (!deviceConnected || !pTxCharacteristic) return;

    // Subcommand 0x16 (22): OBD Info
    uint8_t payload[6];
    payload[0] = 0x16;
    payload[1] = speed;
    payload[2] = (rpm >> 8) & 0xFF;
    payload[3] = rpm & 0xFF;
    payload[4] = coolant;
    payload[5] = (uint8_t)(voltage * 10);

    uint8_t outBuf[64];
    size_t frameLen = buildV221Frame(0x37, 0x16, payload, sizeof(payload), ts, outBuf);

    sendSafeNotify(pTxCharacteristic, outBuf, frameLen);
}

void sendV221Ack(uint8_t cmd, uint8_t sub, uint32_t ts) {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t payload[2] = {0x00, 0x00}; // Status OK
    uint8_t outBuf[32];
    size_t frameLen = buildV221Frame(cmd, sub, payload, sizeof(payload), ts, outBuf);

    sendSafeNotify(pTxCharacteristic, outBuf, frameLen);
}

void sendLegacyHudInfoPacket() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[16];
    pkt[0] = FRAME_HEADER_1;
    pkt[1] = FRAME_HEADER_2;
    pkt[2] = CMD_HUD_INFO;
    pkt[3] = 0x0A;
    pkt[4] = 'H';
    pkt[5] = '1';
    pkt[6] = 'N';
    pkt[7] = 0x01;
    pkt[8] = 0x02;
    pkt[9] = 0x00;
    pkt[10] = 0x00;
    pkt[11] = 138;
    pkt[12] = 0x07;
    pkt[13] = 0x00;
    pkt[14] = calculateChecksum(pkt, 14);

    sendSafeNotify(pTxCharacteristic, pkt, 15);
}

void sendLegacyHeartbeatPacket() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[8];
    pkt[0] = FRAME_HEADER_1;
    pkt[1] = FRAME_HEADER_2;
    pkt[2] = CMD_HEARTBEAT;
    pkt[3] = 0x02;
    pkt[4] = currentSpeed;
    pkt[5] = 0x01;
    pkt[6] = calculateChecksum(pkt, 6);

    sendSafeNotify(pTxCharacteristic, pkt, 7);
}

void sendLegacyObdTelemetryPacket() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[12];
    pkt[0] = FRAME_HEADER_1;
    pkt[1] = FRAME_HEADER_2;
    pkt[2] = CMD_OBD_TELEMETRY;
    pkt[3] = 0x06;
    pkt[4] = currentSpeed;
    pkt[5] = (currentRpm >> 8) & 0xFF;
    pkt[6] = currentRpm & 0xFF;
    pkt[7] = currentCoolantTemp;
    pkt[8] = (uint8_t)(batteryVoltage * 10);
    pkt[9] = 0x00;
    pkt[10] = calculateChecksum(pkt, 10);

    sendSafeNotify(pTxCharacteristic, pkt, 11);
}

// =============================================================================
// SERIAL CLI
// =============================================================================
void handleSerialCLI() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (line.equalsIgnoreCase("help")) {
        Serial.println("\n========== VIETMAP HUD CLONE - DANH SACH LENH ==========");
        Serial.println("  status       : Xem trang thai ket noi BLE & thong so");
        Serial.println("  speed <kmh>  : Gia lap van toc xe (Vi du: speed 60)");
        Serial.println("  info         : Phat lai goi tin V221 Version Info");
        Serial.println("  ping         : Gui goi tin HUD Status 0x53 Keepalive");
        Serial.println("  obd          : Gui thong so OBD (RPM, Nuoc, Binh ac quy)");
        Serial.println("  limit <kmh>  : Thiet lap gioi han toc do (Vi du: limit 80)");
        Serial.println("========================================================\n");
    }
    else if (line.equalsIgnoreCase("status")) {
        Serial.println("\n----------------- TRANG THAI HIEN TAI -----------------");
        Serial.printf("  Ket noi BLE       : %s\n", deviceConnected ? "DA KET NOI (CONNECTED)" : "DANG CHO KET NOI (ADVERTISING)");
        Serial.printf("  Ten thiet bi      : %s\n", DEVICE_NAME);
        Serial.printf("  Goi tin nhan (RX) : %u\n", totalPacketsRx);
        Serial.printf("  Goi tin gui (TX)  : %u\n", totalPacketsTx);
        Serial.printf("  Van toc gia lap   : %u km/h\n", currentSpeed);
        Serial.printf("  Toc do gioi han   : %u km/h\n", currentSpeedLimit);
        Serial.printf("  Canh bao Camera   : %s (%u m)\n", lastCameraAlert.c_str(), lastCameraDistance);
        Serial.printf("  Dieu huong nga re : %s (%u m)\n", lastNavDirection.c_str(), lastNavDistance);
        Serial.printf("  Dien ap ac quy    : %.1f V | Nhiet do nuoc: %d deg C\n", batteryVoltage, currentCoolantTemp);
        Serial.println("-------------------------------------------------------\n");
    }
    else if (line.startsWith("speed ")) {
        int sp = line.substring(6).toInt();
        if (sp >= 0 && sp <= 250) {
            currentSpeed = sp;
            currentRpm = (sp == 0) ? 800 : (1200 + sp * 25);
            Serial.printf("Da cap nhat van toc xe: %u km/h (RPM: %u)\n", currentSpeed, currentRpm);
            sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
        }
    }
    else if (line.equalsIgnoreCase("info")) {
        sendV221VersionInfo(0xC5);
        Serial.println("Da phat lai goi tin V221 Version Info.");
    }
    else if (line.equalsIgnoreCase("ping")) {
        sendV221HudStatus();
        Serial.println("Da gui nhip tim HUD Status Keepalive.");
    }
    else if (line.equalsIgnoreCase("obd")) {
        sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
        Serial.println("Da phat goi tin OBD Telemetry.");
    }
    else if (line.startsWith("limit ")) {
        currentSpeedLimit = line.substring(6).toInt();
        Serial.printf("Da cap nhat gioi han toc do: %u km/h\n", currentSpeedLimit);
    }
}

// =============================================================================
// RAW ADVERTISING
// =============================================================================
static uint8_t rawAdvData[31];
static uint8_t rawAdvDataLen = 0;
static uint8_t rawScanRespData[31];
static uint8_t rawScanRespDataLen = 0;
static volatile bool advDataSet = false;
static volatile bool scanRspSet = false;

static void beginAdvertisingNow() {
    esp_ble_adv_params_t advParams;
    memset(&advParams, 0, sizeof(advParams));
    advParams.adv_int_min = 0x20;
    advParams.adv_int_max = 0x40;
    advParams.adv_type = ADV_TYPE_IND;
    advParams.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    advParams.channel_map = ADV_CHNL_ALL;
    advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    esp_err_t err = esp_ble_gap_start_advertising(&advParams);
    if (err != ESP_OK) Serial.printf("[BLE] LOI start_advertising: %s\n", esp_err_to_name(err));
}

void startRawAdvertising(const char* name) {
    if (name && strlen(name) > 0) {
        strncpy(currentDevName, name, sizeof(currentDevName) - 1);
        currentDevName[sizeof(currentDevName) - 1] = '\0';
    }
    advDataSet = scanRspSet = false;

    // 1. rawAdvData (Flags + 16-bit UUID 0xFFF0 + Name)
    rawAdvDataLen = 0;
    rawAdvData[rawAdvDataLen++] = 0x02;
    rawAdvData[rawAdvDataLen++] = 0x01;
    rawAdvData[rawAdvDataLen++] = 0x06; // Flags

    rawAdvData[rawAdvDataLen++] = 0x03;
    rawAdvData[rawAdvDataLen++] = 0x03;
    rawAdvData[rawAdvDataLen++] = 0xF0;
    rawAdvData[rawAdvDataLen++] = 0xFF; // Complete 16-bit Service UUID: 0xFFF0

    uint8_t nlen = strlen(currentDevName);
    if (rawAdvDataLen + 2 + nlen <= 31) {
        rawAdvData[rawAdvDataLen++] = nlen + 1;
        rawAdvData[rawAdvDataLen++] = 0x09; // Complete Local Name
        memcpy(&rawAdvData[rawAdvDataLen], currentDevName, nlen);
        rawAdvDataLen += nlen;
    }

    // 2. rawScanRespData (128-bit UUID + Name)
    rawScanRespDataLen = 0;
    rawScanRespData[rawScanRespDataLen++] = 0x11;
    rawScanRespData[rawScanRespDataLen++] = 0x07; // Complete 128-bit Service UUID
    static const uint8_t u128[] = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00 };
    memcpy(&rawScanRespData[rawScanRespDataLen], u128, 16);
    rawScanRespDataLen += 16;

    if (rawScanRespDataLen + 2 + nlen <= 31) {
        rawScanRespData[rawScanRespDataLen++] = nlen + 1;
        rawScanRespData[rawScanRespDataLen++] = 0x09; // Complete Local Name
        memcpy(&rawScanRespData[rawScanRespDataLen], currentDevName, nlen);
        rawScanRespDataLen += nlen;
    }

    esp_ble_gap_set_device_name(currentDevName);
    esp_ble_gap_config_adv_data_raw(rawAdvData, rawAdvDataLen);
    esp_ble_gap_config_scan_rsp_data_raw(rawScanRespData, rawScanRespDataLen);
}

void onGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            advDataSet = true;
            if (scanRspSet) beginAdvertisingNow();
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            scanRspSet = true;
            if (advDataSet) beginAdvertisingNow();
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
            Serial.printf("[BLE] Advertising %s\n", advertising ? "DA BAT (READY)" : "THAT BAI");
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            advertising = false;
            break;
        default:
            break;
    }
}

// =============================================================================
// SETUP & MAIN LOOP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n");
    Serial.println("*****************************************************************");
    Serial.println("*      VIETMAP HUD V221 CLONE & BLE EMULATOR (ESP32-S3)         *");
    Serial.println("*      Model: H1N / H2AS | Protocol: 2.2.1 | Key C Security     *");
    Serial.println("*      Duy tri lien tuc ket noi VietMap Live & Kich hoat Pro    *");
    Serial.println("*****************************************************************");
    Serial.printf("[SETUP] Khoi tao Bluetooth BLE voi ten: %s\n", DEVICE_NAME);

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setMTU(517);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new VietMapServerCallbacks());

    BLEService* pService = pServer->createService(BLEUUID(SERVICE_UUID));

    pTxCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_TX_UUID),
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    pRxCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_RX_UUID),
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pRxCharacteristic->addDescriptor(new BLE2902());
    pRxCharacteristic->setCallbacks(new VietMapRxCallbacks());

    pCfgCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_CFG_UUID),
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pCfgCharacteristic->addDescriptor(new BLE2902());

    pAuxCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_AUX_UUID),
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE
    );
    pAuxCharacteristic->addDescriptor(new BLE2902());

    pService->start();

    BLEDevice::setCustomGapHandler(onGapEvent);
    Serial.printf("[SETUP] Dia chi MAC BLE: %s\n", BLEDevice::getAddress().toString().c_str());
    startRawAdvertising();
    Serial.println("[SETUP] BLE GATT Server da san sang va dang phat quang ba (Advertising)!");
    Serial.println("[SETUP] MO APP VIETMAP LIVE TREN DIEN THOAI / MAY TINH BANG DE KET NOI!");
    Serial.println("[SETUP] Go 'help' de xem danh sach lenh dieu khien.\n");
}

void loop() {
    uint32_t now = millis();

    // DUY TRI KET NOI KHONG NGAT (CONNECTION SUPERVISOR):
    // Gui nhan nhip tim va du lieu xe moi 1000ms
    if (deviceConnected) {
        if (now - lastHeartbeatMs >= 1000) {
            lastHeartbeatMs = now;
            sendV221HudStatus(0x01, (uint32_t)(now / 1000));
            sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage, (uint32_t)(now / 1000));
            sendLegacyHeartbeatPacket();
        }
    }

    // Tu dong khoi phuc Advertising neu mat ket noi
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        startRawAdvertising();
        Serial.println("[BLE] Dang tiep tuc quang ba (Advertising) cho ket noi moi...");
        oldDeviceConnected = deviceConnected;
    }

    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }

    static uint32_t lastAdvLogMs = 0;
    if (!deviceConnected && now - lastAdvLogMs >= 5000) {
        lastAdvLogMs = now;
        Serial.printf("[BLE] %s | Ten %s | MAC %s\n",
                      advertising ? "Dang phat quang ba, cho ket noi..." : "KHONG phat quang ba!",
                      DEVICE_NAME,
                      BLEDevice::getAddress().toString().c_str());
    }

    handleSerialCLI();
    delay(15);
}
