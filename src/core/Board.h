#pragma once

// Board-specific bring-up that has to happen before the display/touch/audio
// drivers start. A no-op on the JC3248W535 (3.5") build; on VietHUD 2.8
// (ES3C28P) it brings up the shared touch/codec I2C bus, resets the FT6336
// and configures the ES8311 codec. Call once, early in setup().
void boardEarlyInit();

// ES3C28P speaker amplifier enable (no-op elsewhere). Kept off while the I2S
// port is torn down/reinstalled so the switch-over doesn't pop.
void boardAmpEnable(bool on);
