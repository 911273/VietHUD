#pragma once
#include <stdint.h>

// Board hardware interface — the ONLY board-specific code the shared product
// code calls. Each board implements it in src/boards/<board>/board.cpp (exactly
// one is compiled per env); its pins and capability flags are in
// include/boards/<board>/board_config.h. See include/boards/board.h.

class Arduino_GFX;

// Bring-up that has to happen before the display/touch/audio drivers start
// (shared buses, codec setup, reset lines). Call once, early in setup().
void boardEarlyInit();

// Speaker amplifier enable. Kept off while the I2S port is torn down and
// reinstalled so the switch-over doesn't pop. No-op where there is no switch.
void boardAmpEnable(bool on);

// The panel driver in its NATIVE orientation (TFT_RES_W x TFT_RES_H).
// display/DisplayDriver.cpp wraps it in an Arduino_Canvas, which owns rotation.
Arduino_GFX *boardCreatePanel();

// Touch controller, sampled by touch/TouchTask.cpp on its own task.
// boardTouchRead() returns logical coordinates for the rotation last set.
bool boardTouchBegin();
void boardTouchSetRotation(uint8_t rotation);
bool boardTouchRead(uint16_t *x, uint16_t *y);

// Physical buttons (boards without touch, BOARD_HAS_TOUCH 0): the raw, un-
// debounced pressed state as a bitmask of BOARD_KEY_*. Touch boards return 0.
// ui/ButtonInput.cpp turns it into short/long/double presses and actions.
enum : uint8_t { BOARD_KEY_LEFT = 1, BOARD_KEY_CENTER = 2, BOARD_KEY_RIGHT = 4 };
uint8_t boardButtonsRaw();
