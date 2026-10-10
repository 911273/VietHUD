#pragma once

#if defined(VIETHUD_BOARD_ES3C28P)
// VietHUD 2.8: ILI9341V native portrait 240x320. Rotation is applied on the
// Arduino_Canvas wrapper exactly as on the 3.5" build (rotation 1/3 = the
// 320x240 landscape the compact dashboard layout is drawn for).
#define TFT_RES_W 240
#define TFT_RES_H 320
#define TFT_SPI_HZ 40000000 // full 240x320 frame = ~31 ms; raise only after checking for corruption on the real panel
#else

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
#endif
