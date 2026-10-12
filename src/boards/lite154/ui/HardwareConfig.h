#pragma once
// Pin names the Lite screen code (ported from the stand-alone Lite firmware,
// apps/lite154) uses, mapped onto the board config — one source of pins.
#include <Arduino.h>
#include "boards/board.h"

#define PIN_LCD_BL   TFT_BL
#define PIN_LCD_CS   TFT_CS
#define PIN_LCD_DC   TFT_DC
#define PIN_LCD_RST  TFT_RST
#define PIN_LCD_SCK  TFT_SCK
#define PIN_LCD_MOSI TFT_MOSI
#define LCD_WIDTH    TFT_RES_W
#define LCD_HEIGHT   TFT_RES_H

// Battery (same wiring as the board's xiaozhi firmware power_manager.h)
#define PIN_BAT_ADC      17 // ADC2_CH6, battery voltage divider
#define PIN_BAT_CHARGING 38 // HIGH while the charger is charging

// Buttons (active low, internal pull-up)
#define PIN_BTN_BOOT   BTN_CENTER_PIN
#define PIN_BTN_VOL_UP BTN_RIGHT_PIN
#define PIN_BTN_VOL_DN BTN_LEFT_PIN
