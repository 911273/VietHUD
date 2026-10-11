/**
 * @file main_vietmap_hud_classic.cpp
 * @brief VietMap Hardware HUD Clone for ESP32 Classic (ESP32-WROOM-32 / NodeMCU-32S)
 * 
 * Uses Bluetooth Classic BR/EDR RFCOMM SPP (Serial Port Profile, UUID 0x1101)
 * to clone VietMap HUD H1N and activate VietMap Live Pro on Android.
 * 
 * Target Hardware: ESP32 Classic (DOIT DevKit V1, NodeMCU-32S, ESP32-WROOM-32)
 * Profile: Bluetooth Classic SPP (BluetoothSerial)
 * Device Name: VIETMAP_HUD
 */

#include <Arduino.h>
#include "BluetoothSerial.h"
#include "mbedtls/md5.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run make menuconfig to enable it
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error Bluetooth SPP is not enabled!
#endif

#ifndef LED_BUILTIN
  #define LED_BUILTIN 2
#endif

#define DEVICE_NAME "VIETMAP_HUD"

#define FRAME_HEADER_1      0x55
#define FRAME_HEADER_2      0xAA

#define CMD_HEARTBEAT       0x01
#define CMD_HUD_INFO        0x02
#define CMD_OBD_TELEMETRY   0x03
#define CMD_SPEED_LIMIT     0x04
#define CMD_CAMERA_ALERT    0x05
#define CMD_NAVIGATION      0x06
#define CMD_TPMS_INFO       0x07
#define CMD_ACK             0xFF

static const uint8_t KEY_C[16] = {
    0xF7, 0xF8, 0x15, 0x75, 0x02, 0xDA, 0x52, 0x8E, 
    0xF0, 0xFC, 0x31, 0x39, 0x72, 0x2C, 0xE9, 0xE2
};

BluetoothSerial SerialBT;

bool clientConnected = false;
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

static uint8_t rxBuffer[512];
static size_t rxBufferLen = 0;

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
    outBuf[offset + 2] = (sumB >> 8) & 0xFF;
    outBuf[offset + 3] = sumB & 0xFF;

    outBuf[offset + 4] = (sumC >> 24) & 0xFF;
    outBuf[offset + 5] = (sumC >> 16) & 0xFF;
    outBuf[offset + 6] = (sumC >> 8) & 0xFF;
    outBuf[offset + 7] = sumC & 0xFF;

    return totalLen;
}

uint8_t calculateChecksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

void sendBtPacket(const uint8_t* data, size_t len) {
    if (!SerialBT.hasClient()) return;
    SerialBT.write(data, len);
    totalPacketsTx++;
}

void sendV221VersionInfo(uint8_t reqSub = 0xC5, uint32_t ts = 0) {
    const char* verStr = "MODEL:H1N,HW:1.0,FW:1.0,PROTOCOL:2.2.1,OBDV:1.0_0,SID:12345678";
    size_t strLen = strlen(verStr);

    uint8_t payload[1 + strLen];
    payload[0] = 0x0E;
    memcpy(&payload[1], verStr, strLen);

    uint8_t outBuf[128];
    size_t frameLen = buildV221Frame(0x37, reqSub, payload, sizeof(payload), ts, outBuf);
    sendBtPacket(outBuf, frameLen);
    Serial.printf("[TX V221] >>> Version Info (%s) <<<\n", verStr);
}

void sendV221HudStatus(uint8_t statusByte = 0x01, uint32_t ts = 0) {
    uint8_t payload[4] = {0x53, 0x15, statusByte, 0x00};
    uint8_t outBuf[64];
    size_t frameLen = buildV221Frame(0x37, 0x53, payload, sizeof(payload), ts, outBuf);
    sendBtPacket(outBuf, frameLen);
}

void sendV221ObdTelemetry(uint8_t speed, uint16_t rpm, uint8_t coolant, float voltage, uint32_t ts = 0) {
    uint8_t payload[6];
    payload[0] = 0x16;
    payload[1] = speed;
    payload[2] = (rpm >> 8) & 0xFF;
    payload[3] = rpm & 0xFF;
    payload[4] = coolant;
    payload[5] = (uint8_t)(voltage * 10);

    uint8_t outBuf[64];
    size_t frameLen = buildV221Frame(0x37, 0x16, payload, sizeof(payload), ts, outBuf);
    sendBtPacket(outBuf, frameLen);
}

void sendV221Ack(uint8_t cmd, uint8_t sub, uint32_t ts = 0) {
    uint8_t payload[2] = {0x00, 0x00};
    uint8_t outBuf[32];
    size_t frameLen = buildV221Frame(cmd, sub, payload, sizeof(payload), ts, outBuf);
    sendBtPacket(outBuf, frameLen);
}

void sendLegacyHudInfoPacket() {
    uint8_t pkt[16] = {FRAME_HEADER_1, FRAME_HEADER_2, CMD_HUD_INFO, 0x0A, 'H', '1', 'N', 0x01, 0x02, 0x00, 0x00, 138, 0x07, 0x00, 0x00};
    pkt[14] = calculateChecksum(pkt, 14);
    sendBtPacket(pkt, 15);
}

void sendLegacyHeartbeatPacket() {
    uint8_t pkt[8] = {FRAME_HEADER_1, FRAME_HEADER_2, CMD_HEARTBEAT, 0x02, currentSpeed, 0x01, 0x00};
    pkt[6] = calculateChecksum(pkt, 6);
    sendBtPacket(pkt, 7);
}

void parsePacket(const uint8_t* data, size_t len) {
    if (len < 2) return;

    if (data[0] == 0xA5 && data[1] == 0x5A) {
        uint8_t cmd = (len >= 3) ? data[2] : 0x00;
        uint8_t sub = (len >= 4) ? data[3] : 0x00;
        uint32_t ts = 0;
        if (len >= 12) {
            ts = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
        }

        Serial.printf("[RX V221] CMD: 0x%02X, SUB: 0x%02X, TS: %u (Len: %d)\n", cmd, sub, ts, len);

        if (cmd == 0x37 && sub == 0xC5) {
            Serial.println("  [HANDSHAKE 0xC5] App yeu cau Version Info -> Gui Version Info H1N");
            sendV221VersionInfo(0xC5, ts);
            delay(20);
            sendV221HudStatus(0x01, ts);
            return;
        }

        if (cmd == 0x37 && sub == 0xC3) {
            sendV221Ack(0x37, 0xC3, ts);
            delay(15);
            sendV221HudStatus(0x01, ts);
            return;
        }

        if (len >= 13 && data[12] == 0x02) {
            sendV221HudStatus(0x01, ts);
            return;
        }

        if (len >= 13) {
            uint8_t subType = data[12];
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

    if (data[0] == FRAME_HEADER_1 && data[1] == FRAME_HEADER_2) {
        uint8_t cmd = data[2];
        if (cmd == CMD_HEARTBEAT) sendLegacyHeartbeatPacket();
        else if (cmd == CMD_HUD_INFO) sendLegacyHudInfoPacket();
    }
}

void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    switch (event) {
        case ESP_SPP_SRV_OPEN_EVT:
            clientConnected = true;
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
            delay(30);
            sendLegacyHudInfoPacket();
            break;

        case ESP_SPP_CLOSE_EVT:
            clientConnected = false;
            digitalWrite(LED_BUILTIN, LOW);
            Serial.println("\n[SPP] Dien thoai da ngat ket noi Bluetooth SPP!");
            break;

        default:
            break;
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    delay(1000);

    Serial.println("\n");
    Serial.println("*****************************************************************");
    Serial.println("*    VIETMAP HUD CLONE FOR ESP32 CLASSIC (BLUETOOTH SPP RFCOMM)*");
    Serial.println("*    Model: VIETMAP_HUD (H1N) | Giai ma hoan toan VietMap Live  *");
    Serial.println("*    Chuan Bluetooth Classic BR/EDR SPP 0x1101 cho Android       *");
    Serial.println("*****************************************************************");

    SerialBT.register_callback(btCallback);

    if (!SerialBT.begin(DEVICE_NAME)) {
        Serial.println("[LOI] Khoi tao Bluetooth Classic SPP that bai!");
        while (1) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(100);
        }
    }

    Serial.printf("[SETUP] Bluetooth Classic SPP da san sang voi ten: \"%s\"\n", DEVICE_NAME);
    Serial.println("[SETUP] Hay mo Bluetooth tren dien thoai / may tinh bang va ket noi!");
}

void loop() {
    uint32_t now = millis();

    while (SerialBT.available()) {
        uint8_t b = SerialBT.read();
        rxBuffer[rxBufferLen++] = b;
        if (rxBufferLen >= sizeof(rxBuffer)) rxBufferLen = 0;
    }

    if (rxBufferLen >= 2) {
        if (rxBuffer[0] == 0xA5 && rxBuffer[1] == 0x5A) {
            if (rxBufferLen >= 8) {
                uint32_t dataLen = ((uint32_t)rxBuffer[4] << 24) | ((uint32_t)rxBuffer[5] << 16) | ((uint32_t)rxBuffer[6] << 8) | rxBuffer[7];
                size_t frameTotal = dataLen + 8;
                if (rxBufferLen >= frameTotal) {
                    totalPacketsRx++;
                    parsePacket(rxBuffer, frameTotal);
                    size_t rem = rxBufferLen - frameTotal;
                    memmove(rxBuffer, &rxBuffer[frameTotal], rem);
                    rxBufferLen = rem;
                }
            }
        } else if (rxBuffer[0] == FRAME_HEADER_1 && rxBuffer[1] == FRAME_HEADER_2) {
            if (rxBufferLen >= 4) {
                uint8_t pLen = rxBuffer[3];
                size_t total = pLen + 5;
                if (rxBufferLen >= total) {
                    totalPacketsRx++;
                    parsePacket(rxBuffer, total);
                    size_t rem = rxBufferLen - total;
                    memmove(rxBuffer, &rxBuffer[total], rem);
                    rxBufferLen = rem;
                }
            }
        } else {
            memmove(rxBuffer, &rxBuffer[1], rxBufferLen - 1);
            rxBufferLen--;
        }
    }

    if (SerialBT.hasClient()) {
        if (now - lastHeartbeatMs >= 1000) {
            lastHeartbeatMs = now;
            sendV221HudStatus(0x01, (uint32_t)(now / 1000));
            sendV221ObdTelemetry(currentSpeed, currentRpm, currentCoolantTemp, batteryVoltage);
            sendLegacyHeartbeatPacket();
        }
    }

    if (!SerialBT.hasClient()) {
        static uint32_t lastBlinkMs = 0;
        if (now - lastBlinkMs >= 500) {
            lastBlinkMs = now;
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }

        static uint32_t lastLogMs = 0;
        if (now - lastLogMs >= 3000) {
            lastLogMs = now;
            Serial.printf("[SPP] Dang phat song Bluetooth Classic cho ket noi... | Ten: %s\n", DEVICE_NAME);
        }
    }

    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.equalsIgnoreCase("status")) {
            Serial.printf("Status: %s | Rx: %u | Tx: %u | Speed: %u km/h\n", 
                          SerialBT.hasClient() ? "CONNECTED" : "WAITING", 
                          totalPacketsRx, totalPacketsTx, currentSpeed);
        }
        else if (line.startsWith("speed ")) {
            currentSpeed = line.substring(6).toInt();
            Serial.printf("Van toc: %u km/h\n", currentSpeed);
            sendV221ObdTelemetry(currentSpeed, 800 + currentSpeed * 25, currentCoolantTemp, batteryVoltage);
        }
    }

    delay(10);
}
