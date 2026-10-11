/**
 * @file main_vietmap_hud_emulator.cpp
 * @brief VietMap Hardware HUD Clone & BLE Emulator for ESP32-S3
 * 
 * Clones VietMap HUD (Models: H1N, H2AS, H1X, VIETMAP_HUD) to unlock and maintain
 * VietMap Live Pro features on Android tablet without needing to sit in a car.
 * 
 * Hardware: ESP32-S3 (JC3248W535 / Generic ESP32-S3 N16R8)
 * BLE Service: 0xFFF0 (Custom VietMap HUD Protocol)
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

// =============================================================================
// BLUETOOTH BLE DEFINITIONS & UUIDS
// =============================================================================
#define DEVICE_NAME         "VIETMAP_HUD"          // Prefix 'H1N' triggers hudH1N / H1NewProtocol
#define DEVICE_NAME_ALT     "VIETMAP_HUD"

// Custom VietMap GATT UUIDs (16-bit mapped to 128-bit Bluetooth Base)
#define SERVICE_UUID        "0000fff0-0000-1000-8000-00805f9b34fb"
#define CHAR_TX_UUID        "0000fff1-0000-1000-8000-00805f9b34fb" // Notify / Read to App
#define CHAR_RX_UUID        "0000fff2-0000-1000-8000-00805f9b34fb" // Write from App
#define CHAR_CFG_UUID       "0000fff3-0000-1000-8000-00805f9b34fb" // Config Read/Write
#define CHAR_AUX_UUID       "0000fff4-0000-1000-8000-00805f9b34fb" // Aux Notify

// Frame Header & Protocol Constants
#define FRAME_HEADER_1      0x55
#define FRAME_HEADER_2      0xAA

// Command IDs
#define CMD_HEARTBEAT       0x01  // Heartbeat / Status keepalive
#define CMD_HUD_INFO        0x02  // HUD Device Info / Auth Handshake
#define CMD_OBD_TELEMETRY   0x03  // Vehicle speed, RPM, Voltage, Coolant
#define CMD_SPEED_LIMIT     0x04  // Speed limit & Road sign alerts
#define CMD_CAMERA_ALERT    0x05  // Camera warning (Speed, Penalty, Red Light)
#define CMD_NAVIGATION      0x06  // Turn-by-turn direction & distance
#define CMD_TPMS_INFO       0x07  // Tire pressure data
#define CMD_ACK             0xFF  // Acknowledge response

// Global State
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
uint8_t currentCoolantTemp = 86; // 86°C
float batteryVoltage = 13.8f;    // 13.8V
String lastCameraAlert = "None";
uint16_t lastCameraDistance = 0;
String lastNavDirection = "Straight";
uint16_t lastNavDistance = 0;

// Helper: Calculate Checksum (Sum of bytes & 0xFF)
uint8_t calculateChecksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

// Forward Declarations
void sendHeartbeatPacket();
void sendHudInfoPacket();
void sendHudInfoPacketA5();
void sendObdTelemetryPacketA5();
void sendHeartbeatPacketA5();
void sendObdTelemetryPacket();
// Helper gui notify an toan, tranh tran bo dem BLE stack
void sendSafeNotify(BLECharacteristic* pChar, const uint8_t* data, size_t len) {
    if (!pChar || !deviceConnected) return;
    pChar->setValue((uint8_t*)data, len);
    pChar->notify();
    delay(25); // Cho BLE controller phat song on dinh
}

void parseIncomingPacket(const uint8_t* data, size_t len);
void startRawAdvertising(const char* name = nullptr);
static volatile bool advertising = false;

// =============================================================================
// BLE SERVER CALLBACKS
// =============================================================================
class VietMapServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        deviceConnected = true;
        advertising = false; // the stack stops advertising once a central connects
        Serial.println("\n[BLE] ==================================================");
        Serial.println("[BLE] >>> VIETMAP LIVE APP ĐÃ KẾT NỐI THÀNH CÔNG! <<<");
        Serial.println("[BLE] Đang kích hoạt chế độ VIETMAP PRO...");
        Serial.println("[BLE] ==================================================\n");
        
        delay(100);
        sendHudInfoPacketA5();
        delay(30);
        sendHudInfoPacket();
        delay(30);
        sendObdTelemetryPacketA5();
        delay(30);
        sendObdTelemetryPacket();
    }

    void onDisconnect(BLEServer* pServer) override {
        deviceConnected = false;
        Serial.println("\n[BLE] !!! VIETMAP LIVE APP ĐÃ NGẮT KẾT NỐI !!!");
        Serial.println("[BLE] Khởi động lại BLE Advertising để chờ kết nối mới...");
    }
};

// =============================================================================
// BLE RX CHARACTERISTIC CALLBACKS (Incoming commands from VietMap Live)
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

    // 1. VIETMAP LIVE A5 5A PROTOCOL
    if (data[0] == 0xA5 && data[1] == 0x5A) {
        uint8_t cmd = (len >= 3) ? data[2] : 0x00;
        uint8_t sub = (len >= 4) ? data[3] : 0x00;
        Serial.printf("  -> [VIETMAP LIVE A5 5A] CMD: 0x%02X, SUB: 0x%02X (Total %d bytes)\n", cmd, sub, len);

        if (!pTxCharacteristic) return;

        // XU LY BAT TAY & DONG BO THOI GIAN: CMD 0x37, SUB 0xC3
        if (cmd == 0x37 && sub == 0xC3) {
            uint32_t ts = 0;
            if (len >= 12) {
                ts = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
            }
            Serial.printf("  [HANDSHAKE] Nhan ma dong bo gio iPhone: %u\n", ts);

            // 1. Phan hoi ACK Echo C3
            uint8_t respC3[22];
            memset(respC3, 0, sizeof(respC3));
            respC3[0] = 0xA5;
            respC3[1] = 0x5A;
            respC3[2] = 0x37;
            respC3[3] = 0xC3; // SubCMD C3
            respC3[4] = 0x00;
            respC3[5] = 0x00;
            respC3[6] = 0x00;
            respC3[7] = 0x0E; // 14 bytes payload
            if (len >= 12) memcpy(&respC3[8], &data[8], 4);
            respC3[12] = 0x00; // Status OK
            respC3[13] = 0x00;
            if (len >= 22) memcpy(&respC3[14], &data[14], 8);

            sendSafeNotify(pTxCharacteristic, respC3, sizeof(respC3));
            sendSafeNotify(pRxCharacteristic, respC3, sizeof(respC3));

            // 2. Gui kem SubCMD 0xC4
            respC3[3] = 0xC4;
            sendSafeNotify(pTxCharacteristic, respC3, sizeof(respC3));

            Serial.println("  [TX A5 5A] >>> Da phan hoi HANDSHAKE TIME SYNC (C3/C4) <<<");
            return;
        }

        // XU LY TRUY VAN THONG SO THIET BI: CMD 0x37, SUB 0xC5 (Info Query 0x02)
        if (cmd == 0x37 && sub == 0xC5) {
            Serial.println("  [QUERY] Vietmap Live yeu cau thong tin Model & Hardware Profile (0xC5)...");

            // Goi tin phan hoi Device Info Profile (18 bytes, fit 100% vao BLE MTU)
            uint8_t devProfile[18];
            memset(devProfile, 0, sizeof(devProfile));
            devProfile[0] = 0xA5;
            devProfile[1] = 0x5A;
            devProfile[2] = 0x37;
            devProfile[3] = 0xC5; // SubCMD C5
            devProfile[4] = 0x00;
            devProfile[5] = 0x00;
            devProfile[6] = 0x00;
            devProfile[7] = 0x0A; // 10 bytes payload

            devProfile[8]  = 0x02; // Query ID: 0x02 (Device Info)
            devProfile[9]  = 0x00; // Status: 0 = OK / SUCCESS
            devProfile[10] = 'H';  // Model: H1N
            devProfile[11] = '1';
            devProfile[12] = 'N';
            devProfile[13] = 0x01; // FW Version 1.2.0
            devProfile[14] = 0x02;
            devProfile[15] = 0x00;
            // Echo request tag tu data[10], data[11]
            devProfile[16] = (len >= 12) ? data[10] : 0x3A;
            devProfile[17] = (len >= 12) ? data[11] : 0xA1;

            // Gui voi SubCMD C5 tren ca 2 kenh TX va RX
            sendSafeNotify(pTxCharacteristic, devProfile, sizeof(devProfile));
            sendSafeNotify(pRxCharacteristic, devProfile, sizeof(devProfile));

            // Gui voi SubCMD C6 tren ca 2 kenh
            devProfile[3] = 0xC6;
            sendSafeNotify(pTxCharacteristic, devProfile, sizeof(devProfile));
            sendSafeNotify(pRxCharacteristic, devProfile, sizeof(devProfile));

            // Dong thoi phan hoi goi HUD_INFO A5 va Telemetry
            sendHudInfoPacketA5();
            delay(25);
            sendObdTelemetryPacketA5();

            Serial.println("  [TX A5 5A] >>> Da phan hoi MODEL H1N PROFILE cho C5 & C6 thanh cong! <<<");
            return;
        }

        // PHAN HOI MAC DINH CHO CAC GOI TIN A5 5A KHAC
        uint8_t genericAck[9] = {0xA5, 0x5A, cmd, sub, 0x00, 0x00, 0x00, 0x01, 0x00};
        sendSafeNotify(pTxCharacteristic, genericAck, sizeof(genericAck));
        Serial.printf("  [TX A5 5A] >>> Generic ACK cho CMD 0x%02X, SUB 0x%02X <<<\n", cmd, sub);
        return;
    }

    // 2. LEGACY 55 AA PROTOCOL
    if (data[0] == FRAME_HEADER_1 && data[1] == FRAME_HEADER_2) {
        uint8_t cmd = data[2];
        Serial.printf("  -> [CMD 0x%02X] ", cmd);

        switch (cmd) {
            case CMD_HEARTBEAT:
                Serial.println("App Heartbeat Ping -> Phan hoi ACK");
                sendHeartbeatPacket();
                break;
            case CMD_HUD_INFO:
                Serial.println("App yeu cau xac thuc -> Phan hoi HUD_INFO");
                sendHudInfoPacket();
                break;
            case CMD_SPEED_LIMIT:
                if (len >= 4) {
                    currentSpeedLimit = data[3];
                    Serial.printf("CAP NHAT TOC DO GIOI HAN: %u km/h\n", currentSpeedLimit);
                }
                break;
            case CMD_CAMERA_ALERT:
                if (len >= 6) {
                    uint8_t camType = data[3];
                    lastCameraDistance = (data[4] << 8) | data[5];
                    const char* typeName = "Camera Phat Nguoi";
                    if (camType == 1) typeName = "Camera Ban Toc Do";
                    else if (camType == 2) typeName = "Camera Vuot Den Do";
                    else if (camType == 3) typeName = "Camera Giam Sat Phan Lan";
                    lastCameraAlert = String(typeName);
                    Serial.printf("CANH BAO CAMERA: %s | Khoang cach: %u m\n", typeName, lastCameraDistance);
                }
                break;
            case CMD_NAVIGATION:
                if (len >= 6) {
                    uint8_t navAction = data[3];
                    lastNavDistance = (data[4] << 8) | data[5];
                    Serial.printf("DIEU HUONG: Huong 0x%02X | Con %u m\n", navAction, lastNavDistance);
                }
                break;
            default:
                Serial.println("Lenh VietMap khac -> Tra ve ACK");
                uint8_t ackPkt[] = {FRAME_HEADER_1, FRAME_HEADER_2, CMD_ACK, 0x01, cmd, 0x00};
                ackPkt[5] = calculateChecksum(ackPkt, 5);
                if (pTxCharacteristic) {
                    pTxCharacteristic->setValue(ackPkt, sizeof(ackPkt));
                    pTxCharacteristic->notify();
                    totalPacketsTx++;
                }
                break;
        }
    } else {
        uint8_t ack[] = {0xA5, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
        if (pTxCharacteristic) {
            pTxCharacteristic->setValue(ack, sizeof(ack));
            pTxCharacteristic->notify();
            totalPacketsTx++;
        }
    }
}

// =============================================================================
// PACKET TRANSMITTERS (H1NewProtocol Emulator)
// =============================================================================
void sendHudInfoPacket() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[16];
    pkt[0] = FRAME_HEADER_1;
    pkt[1] = FRAME_HEADER_2;
    pkt[2] = CMD_HUD_INFO;
    pkt[3] = 0x0A; // Payload Length: 10 bytes
    
    // Model "H1N"
    pkt[4] = 'H';
    pkt[5] = '1';
    pkt[6] = 'N';
    
    // Firmware v1.2.0
    pkt[7] = 0x01;
    pkt[8] = 0x02;
    pkt[9] = 0x00;
    
    // Battery Voltage: 13.8V (138 in deci-volts)
    pkt[10] = 0x00;
    pkt[11] = 138;
    
    // Device Status Flags: Bit 0 = OBD Connected, Bit 1 = GPS Ready, Bit 2 = Pro Licensed
    pkt[12] = 0x07;
    pkt[13] = 0x00;
    
    // Checksum
    pkt[14] = calculateChecksum(pkt, 14);

    pTxCharacteristic->setValue(pkt, 15);
    pTxCharacteristic->notify();
    totalPacketsTx++;

    Serial.println("[TX] >>> Đã gửi gói tin HUD_INFO (Model: H1N, FW: 1.2.0, OBD: Ready) <<<");
}

void sendHeartbeatPacket() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[8];
    pkt[0] = FRAME_HEADER_1;
    pkt[1] = FRAME_HEADER_2;
    pkt[2] = CMD_HEARTBEAT;
    pkt[3] = 0x02; // Len
    pkt[4] = currentSpeed;
    pkt[5] = 0x01; // Link alive flag
    pkt[6] = calculateChecksum(pkt, 6);

    pTxCharacteristic->setValue(pkt, 7);
    pTxCharacteristic->notify();
    totalPacketsTx++;
}

void sendObdTelemetryPacket() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[12];
    pkt[0] = FRAME_HEADER_1;
    pkt[1] = FRAME_HEADER_2;
    pkt[2] = CMD_OBD_TELEMETRY;
    pkt[3] = 0x06; // Len
    pkt[4] = currentSpeed;
    pkt[5] = (currentRpm >> 8) & 0xFF;
    pkt[6] = currentRpm & 0xFF;
    pkt[7] = currentCoolantTemp;
    pkt[8] = (uint8_t)(batteryVoltage * 10);
    pkt[9] = 0x00; // Reserved
    pkt[10] = calculateChecksum(pkt, 10);

    pTxCharacteristic->setValue(pkt, 11);
    pTxCharacteristic->notify();
    totalPacketsTx++;
}

// =============================================================================
// SERIAL CLI (Tương tác trực tiếp trên PC qua COM11)
// =============================================================================
void handleSerialCLI() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (line.equalsIgnoreCase("help")) {
        Serial.println("\n========== VIETMAP HUD CLONE - DANH SÁCH LỆNH ==========");
        Serial.println("  status       : Xem trạng thái kết nối BLE & thông số hiện tại");
        Serial.println("  speed <kmh>  : Giả lập vận tốc xe (Ví dụ: speed 60)");
        Serial.println("  info         : Phát lại gói tin HUD_INFO để kích hoạt Pro");
        Serial.println("  ping         : Gửi ngay nhịp tim Heartbeat");
        Serial.println("  obd          : Gửi thông số OBD (RPM, Nhiệt độ, Bình ắc quy)");
        Serial.println("  limit <kmh>  : Thiết lập giới hạn tốc độ (Ví dụ: limit 80)");
        Serial.println("========================================================\n");
    }
    else if (line.equalsIgnoreCase("status")) {
        Serial.println("\n----------------- TRẠNG THÁI HIỆN TẠI -----------------");
        Serial.printf("  Kết nối BLE       : %s\n", deviceConnected ? "ĐÃ KẾT NỐI (CONNECTED)" : "ĐANG CHỜ KẾT NỐI (ADVERTISING)");
        Serial.printf("  Tên thiết bị      : %s\n", DEVICE_NAME);
        Serial.printf("  Gói tin nhận (RX) : %u\n", totalPacketsRx);
        Serial.printf("  Gói tin gửi (TX)  : %u\n", totalPacketsTx);
        Serial.printf("  Vận tốc giả lập   : %u km/h\n", currentSpeed);
        Serial.printf("  Tốc độ giới hạn   : %u km/h\n", currentSpeedLimit);
        Serial.printf("  Cảnh báo Camera   : %s (%u m)\n", lastCameraAlert.c_str(), lastCameraDistance);
        Serial.printf("  Điều hướng ngã rẽ : %s (%u m)\n", lastNavDirection.c_str(), lastNavDistance);
        Serial.printf("  Điện áp ắc quy    : %.1f V | Nhiệt độ nước: %d °C\n", batteryVoltage, currentCoolantTemp);
        Serial.println("-------------------------------------------------------\n");
    }
    else if (line.startsWith("speed ")) {
        int sp = line.substring(6).toInt();
        if (sp >= 0 && sp <= 250) {
            currentSpeed = sp;
            currentRpm = (sp == 0) ? 800 : (1200 + sp * 25);
            Serial.printf("✅ Đã cập nhật vận tốc xe: %u km/h (RPM: %u)\n", currentSpeed, currentRpm);
            sendObdTelemetryPacket();
        }
    }
    else if (line.equalsIgnoreCase("info")) {
        sendHudInfoPacket();
    }
    else if (line.equalsIgnoreCase("ping")) {
        sendHeartbeatPacket();
        Serial.println("✅ Đã gửi nhịp tim Keepalive.");
    }
    else if (line.equalsIgnoreCase("obd")) {
        sendObdTelemetryPacket();
        Serial.println("✅ Đã phát gói tin OBD Telemetry.");
    }
        else if (line.startsWith("name ")) {
        String newName = line.substring(5);
        newName.trim();
        if (newName.length() > 0 && newName.length() <= 16) {
            Serial.printf("✅ Đổi tên Bluetooth thành: \"%s\"\n", newName.c_str());
            startRawAdvertising(newName.c_str());
        }
    }
    else if (line.startsWith("limit ")) {
        currentSpeedLimit = line.substring(6).toInt();
        Serial.printf("✅ Đã cập nhật giới hạn tốc độ: %u km/h\n", currentSpeedLimit);
    }
    else {
        Serial.printf("Lệnh không hợp lệ: \"%s\". Gõ 'help' để xem danh sách lệnh.\n", line.c_str());
    }
}

// =============================================================================
// RAW ADVERTISING (adv data -> scan rsp -> start, sequenced by GAP events)
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

    // 1. Build rawAdvData (Flags + 16-bit UUID + Name)
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

    // 2. Build rawScanRespData (128-bit UUID + Name)
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
    esp_err_t err = esp_ble_gap_config_adv_data_raw(rawAdvData, rawAdvDataLen);
    if (err != ESP_OK) Serial.printf("[BLE] LOI config_adv_data_raw: %s\n", esp_err_to_name(err));
    err = esp_ble_gap_config_scan_rsp_data_raw(rawScanRespData, rawScanRespDataLen);
    if (err != ESP_OK) Serial.printf("[BLE] LOI config_scan_rsp_data_raw: %s\n", esp_err_to_name(err));
}

void onGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            if (param->adv_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS)
                Serial.printf("[BLE] Adv data bi tu choi (status %d)\n", param->adv_data_raw_cmpl.status);
            advDataSet = true;
            if (scanRspSet) beginAdvertisingNow();
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            if (param->scan_rsp_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS)
                Serial.printf("[BLE] Scan response bi tu choi (status %d)\n", param->scan_rsp_data_raw_cmpl.status);
            scanRspSet = true;
            if (advDataSet) beginAdvertisingNow();
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
            Serial.printf("[BLE] Advertising %s (status %d)\n", advertising ? "DA BAT" : "THAT BAI", param->adv_start_cmpl.status);
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            advertising = false;
            break;
        default:
            break;
    }
}


// =============================================================================
// VIETMAP LIVE A5 5A PROTOCOL PACKET TRANSMITTERS
// =============================================================================
void sendHudInfoPacketA5() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[20];
    pkt[0] = 0xA5;
    pkt[1] = 0x5A;
    pkt[2] = 0x02; // CMD_HUD_INFO
    pkt[3] = 0x00; // SEQ
    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x0A; // Payload len = 10 bytes

    pkt[8]  = 'H';
    pkt[9]  = '1';
    pkt[10] = 'N';
    pkt[11] = 0x01; // FW 1.2.0
    pkt[12] = 0x02;
    pkt[13] = 0x00;
    pkt[14] = 0x00;
    pkt[15] = 138;  // 13.8V
    pkt[16] = 0x07; // Status: Bit0=OBD, Bit1=GPS, Bit2=Pro Active
    pkt[17] = 0x00;

    pTxCharacteristic->setValue(pkt, 18);
    pTxCharacteristic->notify();
    totalPacketsTx++;
    Serial.println("[TX A5 5A] >>> Da phan hoi HUD_INFO A5 5A (Model: H1N, Pro Licensed) <<<");
}

void sendHeartbeatPacketA5() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[12];
    pkt[0] = 0xA5;
    pkt[1] = 0x5A;
    pkt[2] = 0x01; // CMD_HEARTBEAT
    pkt[3] = 0x00;
    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x02;
    pkt[8] = currentSpeed;
    pkt[9] = 0x01; // Link alive

    pTxCharacteristic->setValue(pkt, 10);
    pTxCharacteristic->notify();
    totalPacketsTx++;
}

void sendObdTelemetryPacketA5() {
    if (!deviceConnected || !pTxCharacteristic) return;

    uint8_t pkt[16];
    pkt[0] = 0xA5;
    pkt[1] = 0x5A;
    pkt[2] = 0x03; // CMD_OBD_TELEMETRY
    pkt[3] = 0x00;
    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x06;
    pkt[8] = currentSpeed;
    pkt[9] = (currentRpm >> 8) & 0xFF;
    pkt[10] = currentRpm & 0xFF;
    pkt[11] = currentCoolantTemp;
    pkt[12] = (uint8_t)(batteryVoltage * 10);
    pkt[13] = 0x00;

    pTxCharacteristic->setValue(pkt, 14);
    pTxCharacteristic->notify();
    totalPacketsTx++;
}

// =============================================================================
// SETUP & MAIN LOOP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n");
    Serial.println("*****************************************************************");
    Serial.println("*      VIETMAP HUD HARDWARE CLONE & BLE EMULATOR (ESP32-S3)     *");
    Serial.println("*          Giả lập thiết bị phần cứng VietMap H1N / H2AS        *");
    Serial.println("*     Duy trì và Kích hoạt VietMap Live Pro khi test tại bàn     *");
    Serial.println("*****************************************************************");
    Serial.printf("[SETUP] Khởi tạo Bluetooth BLE với tên: %s\n", DEVICE_NAME);

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setMTU(517);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new VietMapServerCallbacks());

    BLEService* pService = pServer->createService(BLEUUID(SERVICE_UUID));

    // Enable FULL Read, Write, Notify, Indicate tren tat ca dac tinh de iOS ket noi tron tru
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
    Serial.println("[SETUP] ✅ BLE GATT Server đã sẵn sàng và đang phát quảng bá (Advertising)!");
    Serial.println("[SETUP] 👉 BẬT BLUETOOTH TRÊN MÁY TÍNH BẢNG VÀ MỞ VIETMAP LIVE ĐỂ KẾT NỐI!");
    Serial.println("[SETUP] Gõ 'help' trên cổng COM11 để xem các lệnh điều khiển.\n");
}

void loop() {
    uint32_t now = millis();

        if (deviceConnected) {
        if (now - lastHeartbeatMs >= 1500) {
            lastHeartbeatMs = now;
            sendHeartbeatPacket();
            sendHeartbeatPacketA5();
            sendObdTelemetryPacketA5();
        }
    }

    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        startRawAdvertising();
        Serial.println("[BLE] Dang tiep tuc quang ba RAW (Advertising) cho thiet bi ket noi lai...");
        oldDeviceConnected = deviceConnected;
    }

    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }

    static uint32_t lastAdvLogMs = 0;
    if (!deviceConnected && now - lastAdvLogMs >= 5000) {
        lastAdvLogMs = now;
        Serial.printf("[BLE] %s | ten H1N | MAC %s\n",
                      advertising ? "Dang phat quang ba, cho ket noi..." : "KHONG phat quang ba!",
                      BLEDevice::getAddress().toString().c_str());
    }

    handleSerialCLI();

    delay(20);
}
