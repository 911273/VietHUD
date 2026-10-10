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
#define DEVICE_NAME         "H1N"          // Prefix 'H1N' triggers hudH1N / H1NewProtocol
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
void sendObdTelemetryPacket();
void parseIncomingPacket(const uint8_t* data, size_t len);

// =============================================================================
// BLE SERVER CALLBACKS
// =============================================================================
class VietMapServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        deviceConnected = true;
        Serial.println("\n[BLE] ==================================================");
        Serial.println("[BLE] >>> VIETMAP LIVE APP ĐÃ KẾT NỐI THÀNH CÔNG! <<<");
        Serial.println("[BLE] Đang kích hoạt chế độ VIETMAP PRO...");
        Serial.println("[BLE] ==================================================\n");
        
        delay(100);
        sendHudInfoPacket();
        delay(50);
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
    if (len < 3) return;

    if (data[0] == FRAME_HEADER_1 && data[1] == FRAME_HEADER_2) {
        uint8_t cmd = data[2];
        Serial.printf("  -> [CMD 0x%02X] ", cmd);

        switch (cmd) {
            case CMD_HEARTBEAT:
                Serial.println("App Heartbeat Ping -> Phản hồi ACK");
                sendHeartbeatPacket();
                break;

            case CMD_HUD_INFO:
                Serial.println("App yêu cầu xác thực thiết bị (verifyHUD) -> Phản hồi HUD_INFO");
                sendHudInfoPacket();
                break;

            case CMD_SPEED_LIMIT:
                if (len >= 4) {
                    currentSpeedLimit = data[3];
                    Serial.printf("CẬP NHẬT TỐC ĐỘ GIỚI HẠN: %u km/h\n", currentSpeedLimit);
                }
                break;

            case CMD_CAMERA_ALERT:
                if (len >= 6) {
                    uint8_t camType = data[3];
                    lastCameraDistance = (data[4] << 8) | data[5];
                    const char* typeName = "Camera Phạt Nguội";
                    if (camType == 1) typeName = "Camera Bắn Tốc Độ";
                    else if (camType == 2) typeName = "Camera Vượt Đèn Đỏ";
                    else if (camType == 3) typeName = "Camera Giám Sát Phân Làn";
                    lastCameraAlert = String(typeName);
                    Serial.printf("CẢNH BÁO CAMERA: %s | Khoảng cách: %u m\n", typeName, lastCameraDistance);
                }
                break;

            case CMD_NAVIGATION:
                if (len >= 6) {
                    uint8_t navAction = data[3];
                    lastNavDistance = (data[4] << 8) | data[5];
                    Serial.printf("ĐIỀU HƯỚNG: Hướng 0x%02X | Còn %u m\n", navAction, lastNavDistance);
                }
                break;

            default:
                Serial.println("Lệnh ứng dụng VietMap khác -> Trả về ACK");
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
        String strMsg = "";
        for (size_t i = 0; i < len; i++) {
            if (data[i] >= 32 && data[i] <= 126) strMsg += (char)data[i];
        }
        if (strMsg.length() > 0) {
            Serial.printf("  -> [RAW TEXT]: \"%s\"\n", strMsg.c_str());
        }
        uint8_t ack[] = {FRAME_HEADER_1, FRAME_HEADER_2, CMD_ACK, 0x00, 0x01};
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
    else if (line.startsWith("limit ")) {
        currentSpeedLimit = line.substring(6).toInt();
        Serial.printf("✅ Đã cập nhật giới hạn tốc độ: %u km/h\n", currentSpeedLimit);
    }
    else {
        Serial.printf("Lệnh không hợp lệ: \"%s\". Gõ 'help' để xem danh sách lệnh.\n", line.c_str());
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
    Serial.println("*      VIETMAP HUD HARDWARE CLONE & BLE EMULATOR (ESP32-S3)     *");
    Serial.println("*          Giả lập thiết bị phần cứng VietMap H1N / H2AS        *");
    Serial.println("*     Duy trì và Kích hoạt VietMap Live Pro khi test tại bàn     *");
    Serial.println("*****************************************************************");
    Serial.printf("[SETUP] Khởi tạo Bluetooth BLE với tên: %s\n", DEVICE_NAME);

    BLEDevice::init(DEVICE_NAME);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new VietMapServerCallbacks());

    BLEService* pService = pServer->createService(BLEUUID(SERVICE_UUID));

    pTxCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_TX_UUID),
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    pRxCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_RX_UUID),
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new VietMapRxCallbacks());

    pCfgCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_CFG_UUID),
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );

    pAuxCharacteristic = pService->createCharacteristic(
        BLEUUID(CHAR_AUX_UUID),
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
    );
    pAuxCharacteristic->addDescriptor(new BLE2902());

    pService->start();

        static const uint8_t rawAdvData[] = {
        0x02, 0x01, 0x06,               // Flags
        0x03, 0x03, 0xF0, 0xFF,         // Complete 16-bit Service UUID: 0xFFF0
        0x04, 0x09, 'H', '1', 'N'       // Complete Local Name: "H1N"
    };
    static const uint8_t rawScanRespData[] = {
        0x11, 0x07, 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00,
        0x04, 0x09, 'H', '1', 'N'
    };
    esp_err_t err;
    err = esp_ble_gap_config_adv_data_raw((uint8_t*)rawAdvData, sizeof(rawAdvData));
    err = esp_ble_gap_config_scan_rsp_data_raw((uint8_t*)rawScanRespData, sizeof(rawScanRespData));
    delay(100);
    esp_ble_adv_params_t advParams;
    memset(&advParams, 0, sizeof(advParams));
    advParams.adv_int_min = 0x20;
    advParams.adv_int_max = 0x40;
    advParams.adv_type = ADV_TYPE_IND;
    advParams.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    advParams.channel_map = ADV_CHNL_ALL;
    advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    err = esp_ble_gap_start_advertising(&advParams);
    Serial.println(err);
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
        }
    }

    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        esp_ble_adv_params_t advParams;
        memset(&advParams, 0, sizeof(advParams));
        advParams.adv_int_min = 0x20;
        advParams.adv_int_max = 0x40;
        advParams.adv_type = ADV_TYPE_IND;
        advParams.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        advParams.channel_map = ADV_CHNL_ALL;
        advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
        esp_ble_gap_start_advertising(&advParams);
        Serial.println("[BLE] Dang tiep tuc quang ba RAW (Advertising) cho thiet bi ket noi lai...");
        oldDeviceConnected = deviceConnected;
    }

    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }

    handleSerialCLI();

    delay(20);
}
