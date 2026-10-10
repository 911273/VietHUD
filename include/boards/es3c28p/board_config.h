#pragma once
// LCDwiki ES3C28P (VietHUD 2.8", ILI9341V 240x320 SPI) — board configuration: pins, panel geometry, identity and the
// capability flags shared code keys off (never off the board name).
// Selected by include/boards/board.h from the env's -DVIETHUD_BOARD_* flag.

// ---- Pins ----
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

// ---- Panel ----
// VietHUD 2.8: ILI9341V native portrait 240x320. Rotation is applied on the
// Arduino_Canvas wrapper exactly as on the 3.5" build (rotation 1/3 = the
// 320x240 landscape the compact dashboard layout is drawn for).
#define TFT_RES_W 240
#define TFT_RES_H 320
#define TFT_SPI_HZ 40000000 // full 240x320 frame = ~31 ms; raise only after checking for corruption on the real panel

// ---- Identity / OTA (core/Version.h) ----
#define BOARD_MODEL       "VietHUD 2.8"
#define BOARD_MODEL_ID    "viethud28"
#define BOARD_FW_CHANNEL  "firmware/viethud28"
#define BOARD_FW_VERSION  "3.1.1" // this model's own release line

// ---- Capabilities (read by shared code instead of board names) ----
#define BOARD_PANEL_SPI_HZ        TFT_SPI_HZ
// Only a landscape (320x240) dashboard exists for this panel, so it boots
// landscape and clampConfig() keeps it there (1 or 3).
#define BOARD_DEFAULT_ROTATION    1
#define BOARD_LANDSCAPE_ONLY      1
// IPS panel with inversion on visibly lifts the lowest RGB565 codes: its
// near-black 0x04060A showed as a tinted grey (2026-10-10) — use pure black.
#define BOARD_DARK_BG             0x000000
#define BOARD_I2S_MCLK_PIN        I2S_MCLK_PIN // ES8311 needs MCLK = 256 x fs
#define BOARD_AUDIO_NAME          "ES8311"
// "P18 Pro" GPS: UBX-only, its real config interface is a Quectel-style
// PQTM subset (see GNSS.cpp) — NMEA is switched off over it at boot.
#define BOARD_HAS_SD_SLOT         1        // microSD on SD_MMC
#define BOARD_HAS_FLASH_DATA      0        // no on-chip data partition
#define BOARD_HAS_TOUCH           1
#define BOARD_GNSS_PQTM           1
