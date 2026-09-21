#pragma once
#include <stdint.h>
#include <stdbool.h>

// Audio driver for JC3248W535 onboard NS4168 I2S Power Amplifier
// Pins: BCLK=GPIO 42, LRCK=GPIO 2, DOUT=GPIO 41
// Provides:
//  - I2S DMA initialization
//  - Crisp PCM tone/chime generation (radar TTC alerts, speed warnings)
//  - Multi-tone alert sequences (Camera chime, Overspeed alarm, Sign notifications)
//  - Volume control (0 - 100%)

void audioInit();
void audioSetVolume(uint8_t percent);
uint8_t audioGetVolume();
void audioPlayTone(uint16_t freqHz, uint16_t durationMs);
void audioPlayCameraAlert();
void audioPlayOverspeedAlert();
void audioPlaySignNotice();
void audioPlayBeep(uint8_t count);
void audioUpdate();
