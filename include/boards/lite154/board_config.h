#pragma once
// VietHUD Lite 1.54" — Xiaozhi / "Xingzhi Cube" ESP32-S3 (N16R8) with an
// ST7789 240x240 IPS (no touch), three buttons, an I2S speaker amp and a power
// latch. No card slot: the data (traffic alerts + voice prompts) lives in an
// on-chip FFat partition (partitions_lite154.csv), uploaded with
// `pio run -e viethud_lite154 -t uploadfs` from data_lite154/ (built by
// tools/build_alerts_pack.py). Pins from the stand-alone Lite firmware
// (apps/lite154/include/HardwareConfig.h).
// Selected by include/boards/board.h from -DVIETHUD_BOARD_LITE154.

// ---- Pins ----
#define PIN_POWER_HOLD  21  // MUST be driven HIGH early or the board powers itself off

#define TFT_BL    13  // backlight, active high
#define TFT_CS    14
#define TFT_DC    8
#define TFT_RST   18
#define TFT_SCK   9
#define TFT_MOSI  10

#define BTN_LEFT_PIN    39  // "Vol-"  (active low, internal pull-up)
#define BTN_CENTER_PIN  0   // "BOOT"
#define BTN_RIGHT_PIN   40  // "Vol+"

#define I2S_BCLK_PIN  15
#define I2S_LRCK_PIN  16
#define I2S_DOUT_PIN  7

// GPS on the UART header — the same "P18 Pro" (UBX-only) module as VietHUD 2.8.
#define GNSS_RX_PIN 44  // ESP32 RX, fed by the GPS module's TX
#define GNSS_TX_PIN 43  // ESP32 TX, drives the GPS module's RX
#define GNSS_BAUD   115200

// ---- Panel ----
#define TFT_RES_W   240
#define TFT_RES_H   240
#define TFT_SPI_HZ  40000000

// ---- Identity / OTA (core/Version.h) ----
#define BOARD_MODEL       "VietHUD Lite"
#define BOARD_MODEL_ID    "viethudlite154"
// New channel for the core-based Lite. Units still on the old stand-alone Lite
// firmware (3.4.x) read the bare /firmware/ root, which stays theirs.
#define BOARD_FW_CHANNEL  "firmware/lite154"
#define BOARD_FW_VERSION  "4.0.0" // 4.x = Lite on the shared VietHUD core

// ---- Capabilities ----
#define BOARD_PANEL_SPI_HZ        TFT_SPI_HZ
#define BOARD_DEFAULT_ROTATION    0
#define BOARD_LANDSCAPE_ONLY      0        // square panel: rotation is cosmetic
#define BOARD_DARK_BG             0x000000 // IPS: pure black, as on the 2.8"
#define BOARD_I2S_MCLK_PIN        -1       // plain I2S amp, no MCLK
#define BOARD_AUDIO_NAME          "I2S amp"
#define BOARD_HAS_SD_SLOT         0
#define BOARD_HAS_FLASH_DATA      1        // FFat partition "ffat"
#define BOARD_HAS_TOUCH           0        // buttons instead (core/Board.h boardButtonsRaw)
#define BOARD_GNSS_PQTM           1
