#pragma once

#if defined(VIETHUD_BOARD_ES3C28P)
// ---------------------------------------------------------------------------
// VietHUD 2.8 — LCDwiki ES3C28P "2.8inch ESP32-S3 Display" (ESP32-S3 N16R8,
// ILI9341V 240x320 SPI, FT6336G cap touch, ES8311 codec + amp, SDIO TF slot).
// Pin map from lcdwiki.com/2.8inch_ESP32-S3_Display, cross-checked 2026-10-10
// against the factory test firmware read off the real board (ES8311, SD_MMC,
// "2.8_ESP32S3_AP" strings). Factory flash backup: backups/viethud28_factory/.
// ---------------------------------------------------------------------------

// LCD (ILI9341V, 4-wire SPI). RST is tied to the ESP32-S3 EN pin.
#define TFT_CS   10
#define TFT_DC   46
#define TFT_SCK  12
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_BL   45 // active high

// Touch (FT6336G) — shares this I2C bus with the ES8311 codec.
#define TOUCH_SDA  16
#define TOUCH_SCL  15
#define TOUCH_RST  18
#define TOUCH_INT  17
#define TOUCH_ADDR 0x38

// Audio: ES8311 codec (I2C 0x18 on the touch bus) + I2S; amp enable is
// active LOW.
#define I2S_MCLK_PIN 4
#define I2S_BCLK_PIN 5
#define I2S_LRCK_PIN 7
#define I2S_DOUT_PIN 8 // ESP32 -> codec DAC (SDIN)
#define I2S_DIN_PIN  6 // codec ADC (mic) -> ESP32, unused
#define AUDIO_AMP_EN_PIN 1
#define ES8311_ADDR 0x18

// GNSS (u-blox M10, SPG 5.10) on the 4-pin UART header — confirmed on the
// real unit 2026-10-10 by sweeping pins/bauds: GPS TX -> GPIO44, GPS RX <-
// GPIO43, 115200 baud, module ships with NMEA output OFF (UBX NAV-PVT only),
// so GNSS.cpp re-enables NMEA via CFG-VALSET at boot.
#define GNSS_RX_PIN 44
#define GNSS_TX_PIN 43
#define GNSS_BAUD   115200

// TF slot on the SD_MMC peripheral, used in 1-bit mode (D1-D3 = 41/48/47
// left unused, same as the JC3248W535 build).
#define SD_MMC_CLK_PIN 38
#define SD_MMC_CMD_PIN 40
#define SD_MMC_D0_PIN  39

#define RGB_LED_PIN  42
#define BATT_ADC_PIN 9

#else // JC3248W535 (VietHUD 3.5")

// JC3248W535 (ESP32-S3-N16R8V) pin mapping — confirmed against real-world
// Arduino_GFX/AXS15231B example projects for this exact board, matches
// docs/radar_car_V1.1_spec.md section 19.

// LCD (AXS15231B, QSPI)
#define TFT_BL   1
#define TFT_CS   45
#define TFT_SCK  47
#define TFT_SDA0 21
#define TFT_SDA1 48
#define TFT_SDA2 40
#define TFT_SDA3 39

// Touch (AXS15231B combo controller, I2C)
#define TOUCH_SDA  4
#define TOUCH_SCL  8
#define TOUCH_INT  3
#define TOUCH_ADDR 0x3B

// GNSS (u-blox M10N, UART2) — real wiring as connected and confirmed by the
// user 2026-09-15: GPS module TX -> GNSS_RX_PIN, GPS module RX -> GNSS_TX_PIN
// (TX/RX crossed, as UART always is). This is the OPPOSITE pin assignment
// from the spec section 19 placeholder (which proposed RX=18/TX=17) — the
// real wiring wins; update this if the harness is ever rewired.
#define GNSS_RX_PIN 17 // ESP32 RX, fed by the GPS module's TX
#define GNSS_TX_PIN 18 // ESP32 TX, drives the GPS module's RX

// Onboard microSD/TF slot — CONFIRMED 2026-09-15 from the vendor-adjacent
// demo project for this exact board (refob/Arduino_JC3248W535_LVGL9.4,
// Arduino/mp3_player/pincfg_JC3248W535.h — its TFT_CS/TFT_SCK/TFT_SDA0-3/
// TOUCH_SDA/TOUCH_SCL/TOUCH_INT values match every other entry in this file
// exactly, confirming it's the same board revision, not a different one).
//
// This REPLACES an earlier guess (SD_CS_PIN=10/SD_MOSI_PIN=11/SD_SCK_PIN=12/
// SD_MISO_PIN=13, from a third-party blog describing it as a 4-wire SPI
// interface) that was flat-out the wrong PROTOCOL, not just wrong pin
// numbers: this board wires the TF slot to the ESP32-S3's dedicated
// SD_MMC peripheral in 1-bit mode (3 signals: CLK/CMD/D0, no CS pin exists
// in this mode at all), not general-purpose SPI. That earlier guess's
// SD.h/SPIClass(HSPI-or-FSPI) approach caused two distinct, confirmed
// real-hardware regressions (touch corruption on HSPI, ~5x slower display
// flush on FSPI) — both were downstream of forcing SD traffic onto the same
// SPI2/SPI3 peripherals the display's QSPI bus and Wire/I2C touch driver
// already depend on. SD_MMC's dedicated hardware peripheral shares neither,
// which is the actual fix, not a different pin guess.
#define SD_MMC_CLK_PIN 12
#define SD_MMC_CMD_PIN 11
#define SD_MMC_D0_PIN  13
#define GNSS_BAUD 38400 // see GNSS.cpp kGnssBaud

#endif
