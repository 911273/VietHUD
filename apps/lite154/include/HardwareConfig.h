#pragma once

#include <Arduino.h>

// =============================================================================
// Xiaozhi AI / Xingzhi Cube 1.54" TFT (ST7789 240x240 Non-Touch)
// =============================================================================

// Power Hold / Latch (GPIO 21 MUST be HIGH to keep board & peripherals powered)
#define PIN_POWER_HOLD  21

// Display ST7789 (4-wire SPI)
#define PIN_LCD_BL      13  // Backlight (Active HIGH)
#define PIN_LCD_CS      14  // Chip Select
#define PIN_LCD_DC      8   // Data / Command
#define PIN_LCD_RST     18  // Reset
#define PIN_LCD_SCK     9   // SPI Clock (SCL)
#define PIN_LCD_MOSI    10  // SPI MOSI (SDA)

#define LCD_WIDTH       240
#define LCD_HEIGHT      240

// Physical Buttons (Active LOW with internal pull-up)
#define PIN_BTN_BOOT    0   // Boot button (Center - Mode toggle)
#define PIN_BTN_VOL_UP  40  // Volume Up button (Right - Speed +10)
#define PIN_BTN_VOL_DN  39  // Volume Down button (Left - Speed -10)

// Speaker Audio I2S
#define PIN_I2S_BCLK    15  // Bit Clock
#define PIN_I2S_LRCK    16  // Word Select / LRCK
#define PIN_I2S_DOUT    7   // Serial Data Out

// External GPS UART (UART1 on free GPIOs)
// Connect GPS Module TX -> ESP32-S3 RX (GPIO 44)
// Connect GPS Module RX -> ESP32-S3 TX (GPIO 43)
#define PIN_GPS_RX      44
#define PIN_GPS_TX      43
#define GPS_BAUD_DEFAULT 9600
