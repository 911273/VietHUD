#pragma once

// Physical-button control for boards without touch (BOARD_HAS_TOUCH 0, e.g.
// VietHUD Lite 1.54"). Call from loop(); a no-op on touch boards (their
// boardButtonsRaw() is always 0). Keeps the stand-alone Lite firmware's
// mapping so existing users don't relearn it:
//   left  short / long  : volume -10 % / sound on-off
//   right short / long  : volume +10 % / brightness 25-50-75-100 %
//   center double       : Wi-Fi on/off
//   center long         : System Update screen (Wi-Fi + versions); short closes it
//   update prompt up    : right or center = update now, left = later
void buttonInputPoll();
