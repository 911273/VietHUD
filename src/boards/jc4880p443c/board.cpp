// VietHUD 4.3 (Guition JC4880P443C, ESP32-P4) — core/Board.h implementation.
// Pins/capabilities: include/boards/jc4880p443c/board_config.h.
// This board brings its own display, touch and audio-out drivers (this folder:
// DisplayDriverP4.cpp, TouchTaskP4.cpp, AudioOutP4.cpp) in place of the
// Arduino_GFX/I2S ones the S3 boards share, so the panel/touch hooks below are
// never called; they exist to satisfy the common interface.
#include "core/Board.h"
#include "boards/board.h"
#include <Arduino.h>

void boardEarlyInit() {
    Serial.println("[board] VietHUD 4.3 (ESP32-P4, ST7701S 800x480 MIPI-DSI, GT911, ES8311)");
}

void boardAmpEnable(bool) {} // AudioOutP4 owns PA_EN_PIN

Arduino_GFX *boardCreatePanel() { return nullptr; } // DisplayDriverP4.cpp

bool boardTouchBegin() { return true; } // TouchTaskP4.cpp
void boardTouchSetRotation(uint8_t) {}
bool boardTouchRead(uint16_t *, uint16_t *) { return false; }

uint8_t boardButtonsRaw() { return 0; }
