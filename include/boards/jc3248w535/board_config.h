#pragma once
// JC3248W535 (VietHUD 3.5", AXS15231B 320x480 QSPI) — board configuration: pins, panel geometry, identity and the
// capability flags shared code keys off (never off the board name).
// Selected by include/boards/board.h from the env's -DVIETHUD_BOARD_* flag.

// ---- Pins ----

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


// ---- Panel ----

// Display
//
// IMPORTANT: gfx->setRotation() called directly on the raw Arduino_AXS15231B
// panel driver corrupts the write address window on this board (confirmed
// on real hardware 2026-09-14: fillScreen() "succeeds" with no error, but
// nothing appears — screen stays blank). The confirmed community reference
// avoids this entirely by only rotating an Arduino_Canvas *wrapper* around
// the panel, never the raw panel driver itself. We sidestep the bug the
// simpler way for now: keep native portrait orientation (rotation 0) and
// lay the UI out in 320x480. Revisit with an Arduino_Canvas wrapper if
// landscape is needed later.
#define TFT_ROTATION 0
#define TFT_RES_W    320
#define TFT_RES_H    480

// Touch calibration offsets (raw controller range -> panel pixel range).
// Values taken from confirmed real-world JC3248W535 example projects.
// Re-calibrate on Diagnostics screen if touch feels off on this specific panel.
#define TOUCH_X_MIN 12
#define TOUCH_X_MAX 310
#define TOUCH_Y_MIN 14
#define TOUCH_Y_MAX 461

// ---- Identity / OTA (core/Version.h) ----
#define BOARD_MODEL       "VietHUD 3.5"
#define BOARD_MODEL_ID    "viethud35"
// Own folder since 2026-10-10. The bare /firmware/ root stays VietHUD Lite's
// until Lite moves onto the shared core (deployed Lite units read it).
#define BOARD_FW_CHANNEL  "firmware/viethud35"
#define BOARD_FW_VERSION  "3.1.0" // this model's own release line

// ---- Capabilities (read by shared code instead of board names) ----
#define BOARD_PANEL_SPI_HZ        GFX_NOT_DEFINED // QSPI driver picks its own clock
#define BOARD_DEFAULT_ROTATION    0
#define BOARD_LANDSCAPE_ONLY      0
#define BOARD_DARK_BG             0x04060A // panel renders near-black fine
#define BOARD_I2S_MCLK_PIN        -1       // NS4168 amp: no MCLK
#define BOARD_AUDIO_NAME          "NS4168"
#define BOARD_GNSS_PQTM           0        // plain u-blox M10 (NMEA)
#define GNSS_BAUD                 38400    // see GNSS.cpp kGnssBaud
#define I2S_BCLK_PIN              42
#define I2S_LRCK_PIN              2
#define I2S_DOUT_PIN              41
