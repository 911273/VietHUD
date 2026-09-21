#pragma once

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

// Radar (HLK-LD2451, UART1) — real wiring as connected and confirmed by
// the user 2026-09-16: "RX-IO5, TX-IO6" (the module's own pin labels),
// i.e. LD2451 RX -> RADAR_TX_PIN, LD2451 TX -> RADAR_RX_PIN, same
// TX/RX-crossed convention as GNSS above. This is close to but not
// identical to the spec section 19 placeholder (RX=6/TX=7) — real wiring
// wins; update this if the harness is ever rewired.
#define RADAR_RX_PIN 6 // ESP32 RX, fed by the LD2451's TX
#define RADAR_TX_PIN 5 // ESP32 TX, drives the LD2451's RX

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
