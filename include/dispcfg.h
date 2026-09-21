#pragma once

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
