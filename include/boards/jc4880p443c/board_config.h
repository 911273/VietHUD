#pragma once
// VietHUD 4.3 — Guition JC4880P443C: ESP32-P4 rev v3.2 + ESP32-C6 (Wi-Fi/BLE
// over SDIO, esp-hosted), ST7701S 480x800 MIPI-DSI, GT911 touch, ES8311 codec,
// 4-bit microSD, 16 MB flash, 32 MB PSRAM. Pin map read LIVE from the board's
// GPIO/IO_MUX registers while its factory fw ran (2026-09-29,
// Projects/_Hardware/esp32p4_JC4880P443C) and cross-checked against the
// schematic (github.com/ultramcu/guition-jc4880p443c-i-w).
//
// Different chip family from the S3 boards: pioarduino (Arduino 3.x / IDF 5)
// toolchain, so the env also defines VIETHUD_P4 — the chip-family switch for
// IDF-5-only APIs (MIPI-DSI + PPA display, i2s_std audio, temperature_sensor,
// C6 BLE). Board drivers live in src/boards/jc4880p443c/ (display, touch,
// audio out, 1.5x fonts); the UI is the shared one, scaled by ui/UiScale.h.
// Selected by include/boards/board.h from -DVIETHUD_BOARD_JC4880P443C.

// ---- Pins ----
// Display is MIPI-DSI (dedicated pins); only reset/backlight are GPIOs.
#define LCD_RST_PIN   5   // ST7701S reset, active low
#define TFT_BL        23  // backlight PWM (LEDC) -> MP3202 boost enable
#define LCD_PANEL_W   480 // native portrait
#define LCD_PANEL_H   800
// Touch GT911 + ES8311 codec share one I2C bus (Wire)
#define TOUCH_SDA     7
#define TOUCH_SCL     8
// Audio: ES8311 over I2S, NS4150 amp enable
#define I2S_MCLK_PIN  13
#define I2S_BCLK_PIN  12
#define I2S_LRCK_PIN  10
#define I2S_DOUT_PIN  9
#define I2S_DIN_PIN   48
#define PA_EN_PIN     11
// microSD: SDMMC slot0 4-bit (IO_MUX pins); TF_VCC comes from on-chip LDO4
#define SD_MMC_CLK_PIN 43
#define SD_MMC_CMD_PIN 44
#define SD_MMC_D0_PIN  39
#define SD_MMC_D1_PIN  40
#define SD_MMC_D2_PIN  41
#define SD_MMC_D3_PIN  42
// GNSS (u-blox M10N, UART2) on expansion header JP1: module TX -> GPIO30
// (JP1 pin 10), module RX <- GPIO31 (JP1 pin 8), 3V3 = pin 1, GND = pin 6.
// Without a module the phone can feed GPS over BLE (net/PhoneGpsBle.h).
#define GNSS_RX_PIN 30
#define GNSS_TX_PIN 31
#define GNSS_BAUD   38400

// ---- Panel (UI design units) ----
// LVGL renders at the native 800x480, but the shared UI is laid out in the
// S3's units and scaled x1.5 by ui/UiScale.h, so the layout code sees a
// 320x533 (portrait) / 533x320 (landscape) screen — the 3.5" class.
#define TFT_RES_W 320
#define TFT_RES_H 480

// ---- Identity / OTA (core/Version.h) ----
#define BOARD_MODEL       "VietHUD 4.3"
#define BOARD_MODEL_ID    "viethud43"
#define BOARD_FW_CHANNEL  "firmware/viethud43"

// ---- Capabilities ----
#define BOARD_PANEL_SPI_HZ        0        // MIPI-DSI, not SPI (unused)
#define BOARD_DEFAULT_ROTATION    1        // portrait panel mounted landscape
#define BOARD_LANDSCAPE_ONLY      0
#define BOARD_DARK_BG             0x000000
#define BOARD_I2S_MCLK_PIN        I2S_MCLK_PIN // ES8311 needs MCLK
#define BOARD_AUDIO_NAME          "ES8311"
#define BOARD_HAS_SD_SLOT         1
#define BOARD_SD_4BIT             1
#define BOARD_SD_LDO_CHAN         4        // TF_VCC
#define BOARD_HAS_FLASH_DATA      1        // core speedmap in the "ffat" partition when no card
#define BOARD_FLASH_DATA_READONLY 1        // writes corrupted that FFat (2026-09-29)
#define BOARD_HAS_TOUCH           1
#define BOARD_GNSS_PQTM           0        // plain u-blox M10 (NMEA)
